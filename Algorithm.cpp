//algorithm to dispaly parking availability
START

1. Get total number of parking slots.
2. Count the slots whose status is AVAILABLE.
3. Display every slot and its status.
4. Display total available slots.
5. IF available slots > 0
       Display "PARKING AVAILABLE"
   ELSE
       Display "PARKING FULL"
6. END IF

STOP
//algorithm for vehicle registration
START

1. Receive vehicle registration number.
2. Receive vehicle type.
3. Check whether the registration number already exists.
4. IF vehicle already exists
       Display "Vehicle already registered/parked"
       STOP
5. ELSE
       Continue
6. Find an available parking slot.
7. Record vehicle information.
8. Record entry time.
9. Assign parking slot.
10. Change slot status to OCCUPIED.
11. Decrease available-slot count by 1.
12. Display confirmation.

STOP
//slot alocation algorithm
START

FOR every parking slot

    IF slot status == AVAILABLE

        Assign slot to vehicle

        Change status to OCCUPIED

        RETURN slot number

    END IF

END FOR

Display "No slot available"

STOP
//algorithm to calculate parking time
START

1. Retrieve vehicle entry time.
2. Record current exit time.
3. Calculate:

   duration = exit time - entry time

4. Convert duration into minutes.
5. Return duration.

STOP
//algoritm to calculate parking fee
START

INPUT duration in minutes

IF duration <= 30
    fee = 0

ELSE IF duration <= 120
    fee = 50

ELSE IF duration <= 240
    fee = 100

ELSE IF duration <= 360
    fee = 300

ELSE
    fee = 500

END IF

RETURN fee

STOP
//algorithm for payment
START

1. Display parking fee.
2. Receive payment.
3. Check payment amount.

IF payment is successful

    Set payment status to PAID
    Display "Payment successful"

ELSE

    Set payment status to UNPAID
    Display "Payment failed"

END IF

STOP
//algorithm for exit barrier
START

Check payment status

IF payment status == PAID

    Open barrier
    Allow vehicle to exit

ELSE

    Keep barrier closed
    Display "Payment required"

END IF

STOP
//algorithm of release of parking slot
START

1. Identify vehicle's parking slot.
2. Change slot status to AVAILABLE.
3. Remove vehicle from active parking list.
4. Increase available-slot count by 1.
5. Update visual display.

STOP