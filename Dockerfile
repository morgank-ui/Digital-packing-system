FROM gcc:13-bookworm AS build
RUN apt-get update && apt-get install -y --no-install-recommends libsqlite3-dev && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY . .
RUN g++ -std=c++17 -O2 -Wall -Wextra -pthread src/main.cpp -o parksmart -lsqlite3

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends libsqlite3-0 && rm -rf /var/lib/apt/lists/* && mkdir -p /data
WORKDIR /app
COPY --from=build /app/parksmart ./parksmart
COPY web ./web
EXPOSE 8080
CMD ["sh", "-c", "./parksmart --host 0.0.0.0 --port ${PORT:-8080} --db /data/parksmart.db --web web"]
