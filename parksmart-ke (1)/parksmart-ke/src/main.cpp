// ============================================================================
//  ParkSmart KE  -  Automated Parking System (C++17)
//  main.cpp      -  Web server: serves the web page and the REST API
//
//  Architecture
//  ------------
//     Browser (web/index.html)  --HTTP-->  main.cpp (routes)
//                                              |
//                                        ParkingService  (business rules)
//                                         |      |      |
//                                 SlotManager  FeeCalculator  Database (SQLite)
//
//  REST API (all responses are JSON)
//     GET  /api/config                    -> app name, demo flag
//     GET  /api/status                    -> slots, counts per zone, tariff
//     POST /api/entry                     -> plate [, slot_id]         issue ticket
//     GET  /api/exit/quote?plate=         -> time spent + amount due
//     POST /api/exit/pay                  -> plate, method [, reference, amount]
//     POST /api/exit/barrier              -> plate                      open barrier
//     --- manager only (header  X-Admin-Key: <password>) ---
//     GET  /api/admin/tickets             -> vehicles currently inside
//     GET  /api/admin/history?limit=      -> completed visits
//     GET  /api/admin/report              -> today's figures
//     GET  /api/admin/events              -> recent activity
//     POST /api/admin/slots               -> zone, count                add bays
//     POST /api/admin/maintenance         -> slot_id, on=1|0
//     POST /api/admin/simulate            -> plate, minutes   (only with --demo)
//
//  Run:  ./parksmart --port 8080 --db parksmart.db --web web [--demo]
//  Manager password: environment variable PARK_ADMIN_PASSWORD (default "admin123").
// ============================================================================
#include <csignal>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>

#include "../third_party/httplib.h"
#include "ParkingService.hpp"

namespace {

const char* const kAppName = "ParkSmart KE";

httplib::Server* g_server = nullptr;  // so Ctrl+C can stop the server cleanly
void onSignal(int) {
    if (g_server) g_server->stop();
}

struct Options {
    std::string host = "127.0.0.1";  // use --host 0.0.0.0 to reach it from other devices
    int port = 8080;
    std::string dbPath = "parksmart.db";
    std::string webDir = "web";
    bool demo = false;
};

Options parseArgs(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) throw std::runtime_error("Missing value after " + a);
            return argv[++i];
        };
        if (a == "--port") o.port = std::stoi(next());
        else if (a == "--host") o.host = next();
        else if (a == "--db") o.dbPath = next();
        else if (a == "--web") o.webDir = next();
        else if (a == "--demo") o.demo = true;
        else if (a == "--help" || a == "-h") {
            std::cout << "Usage: parksmart [--port 8080] [--host 127.0.0.1] [--db parksmart.db]\n"
                         "                 [--web web] [--demo]\n";
            std::exit(0);
        } else throw std::runtime_error("Unknown option: " + a);
    }
    return o;
}

// Read an integer form/query parameter; `fallback` if it is missing or empty.
int intParam(const httplib::Request& req, const char* name, int fallback) {
    if (!req.has_param(name)) return fallback;
    const std::string v = req.get_param_value(name);
    if (v.empty()) return fallback;
    try {
        std::size_t used = 0;
        int n = std::stoi(v, &used);
        if (used != v.size()) throw std::invalid_argument("trailing characters");
        return n;
    } catch (...) {
        throw ApiError(400, std::string("'") + name + "' must be a whole number.");
    }
}

std::string strParam(const httplib::Request& req, const char* name) {
    return req.has_param(name) ? req.get_param_value(name) : std::string();
}

using Handler = std::function<std::string(const httplib::Request&)>;

// Wrap a handler so that (a) its JSON result is sent and (b) any ApiError becomes a
// proper HTTP status + {"ok":false,"error":"..."} body. Unexpected exceptions are
// logged and reported as a generic 500 so internals never leak to the browser.
std::function<void(const httplib::Request&, httplib::Response&)> route(Handler handler) {
    return [handler](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Cache-Control", "no-store");
        try {
            res.set_content(handler(req), "application/json");
        } catch (const ApiError& e) {
            res.status = e.status;
            res.set_content(json::Object().set("ok", false).set("error", e.what()).dump(),
                            "application/json");
        } catch (const std::exception& e) {
            std::cerr << "[error] " << req.method << " " << req.path << ": " << e.what() << "\n";
            res.status = 500;
            res.set_content(json::Object()
                                .set("ok", false)
                                .set("error", "Something went wrong on the server. Please try again.")
                                .dump(),
                            "application/json");
        }
    };
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Options opt = parseArgs(argc, argv);
        const char* pw = std::getenv("PARK_ADMIN_PASSWORD");
        const std::string adminPassword = pw ? pw : "admin123";

        ParkingService service(opt.dbPath, opt.demo);

        httplib::Server svr;
        g_server = &svr;
        std::signal(SIGINT, onSignal);
        std::signal(SIGTERM, onSignal);

        // Manager-only routes call this first. (Simple shared-password check for a
        // course project; a production system would use hashed passwords + sessions
        // over HTTPS.)
        auto requireAdmin = [&](const httplib::Request& req) {
            if (req.get_header_value("X-Admin-Key") != adminPassword)
                throw ApiError(401, "Manager password required.");
        };

        // ---------------- public routes (drivers & gate kiosks) ----------------
        svr.Get("/api/config", route([&](const httplib::Request&) {
            return json::Object().set("ok", true).set("name", kAppName).set("demo", service.demoMode()).dump();
        }));

        // Slot map + counts + tariff: the "visual display" drivers see before entry.
        svr.Get("/api/status", route([&](const httplib::Request&) { return service.status(); }));

        // Vehicle arrives: record plate, allocate a slot, issue a ticket.
        svr.Post("/api/entry", route([&](const httplib::Request& req) {
            return service.registerEntry(strParam(req, "plate"), intParam(req, "slot_id", 0));
        }));

        // Vehicle at the exit: how long, how much?
        svr.Get("/api/exit/quote", route([&](const httplib::Request& req) {
            return service.quote(strParam(req, "plate"));
        }));

        // Pay the amount due (M-Pesa / cash / card).
        svr.Post("/api/exit/pay", route([&](const httplib::Request& req) {
            return service.pay(strParam(req, "plate"), strParam(req, "method"),
                               strParam(req, "reference"), intParam(req, "amount", -1));
        }));

        // Open the exit barrier - refused with 402 unless everything is paid.
        svr.Post("/api/exit/barrier", route([&](const httplib::Request& req) {
            return service.openBarrier(strParam(req, "plate"));
        }));

        // ---------------- manager routes ----------------
        svr.Get("/api/admin/tickets", route([&](const httplib::Request& req) {
            requireAdmin(req);
            return service.activeTickets();
        }));
        svr.Get("/api/admin/history", route([&](const httplib::Request& req) {
            requireAdmin(req);
            return service.history(intParam(req, "limit", 20));
        }));
        svr.Get("/api/admin/report", route([&](const httplib::Request& req) {
            requireAdmin(req);
            return service.report();
        }));
        svr.Get("/api/admin/events", route([&](const httplib::Request& req) {
            requireAdmin(req);
            return service.events();
        }));
        svr.Post("/api/admin/slots", route([&](const httplib::Request& req) {
            requireAdmin(req);
            return service.addSlots(strParam(req, "zone"), intParam(req, "count", 1));
        }));
        svr.Post("/api/admin/maintenance", route([&](const httplib::Request& req) {
            requireAdmin(req);
            return service.setMaintenance(intParam(req, "slot_id", 0), strParam(req, "on") == "1");
        }));
        svr.Post("/api/admin/simulate", route([&](const httplib::Request& req) {
            requireAdmin(req);
            return service.simulateElapsed(strParam(req, "plate"), intParam(req, "minutes", 0));
        }));

        // ---------------- static files (the web page) ----------------
        if (!svr.set_mount_point("/", opt.webDir)) {
            std::cerr << "Cannot find web folder '" << opt.webDir
                      << "'. Run from the project root or pass --web <path>.\n";
            return 1;
        }

        std::cout << kAppName << " running at http://" << opt.host << ":" << opt.port << "\n"
                  << "  database : " << opt.dbPath << "\n"
                  << "  web root : " << opt.webDir << "\n"
                  << "  demo mode: " << (opt.demo ? "ON (time-travel tools enabled)" : "off") << "\n";
        if (!pw) std::cout << "  WARNING  : using the default manager password 'admin123'. "
                              "Set PARK_ADMIN_PASSWORD before real use.\n";
        std::cout << "Press Ctrl+C to stop.\n";

        if (!svr.listen(opt.host, opt.port)) {
            std::cerr << "Could not listen on " << opt.host << ":" << opt.port
                      << " (is the port already in use?)\n";
            return 1;
        }
        std::cout << "\nServer stopped.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Fatal: " << e.what() << "\n";
        return 1;
    }
}
