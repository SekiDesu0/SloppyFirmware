import serial
import threading
import sys
import time

# --- Configuration ---
COM_PORT = 'COM5' 
BAUD_RATE = 115200

def read_from_port(ser):
    """Background thread to continuously read and print incoming serial data."""
    while ser.is_open:
        try:
            if ser.in_waiting > 0:
                reading = ser.read(ser.in_waiting).decode('utf-8', errors='replace')
                print(reading, end='')
            time.sleep(0.01)
        except Exception as e:
            print(f"\n[!] Serial read error: {e}")
            break

def main():
    try:
        # Instead of opening immediately, instantiate the object first
        ser = serial.Serial()
        ser.port = COM_PORT
        ser.baudrate = BAUD_RATE
        ser.timeout = 1
        
        # The SlimeVR magic trick: release the reset pins
        ser.dtr = False 
        ser.rts = False 
        
        # Now open the port
        ser.open()
        
        print(f"[*] Connected to {COM_PORT} at {BAUD_RATE} baud.")
        print("[*] Type 'quit' or 'exit' to close the console.")
        print("-" * 50)
    except serial.SerialException as e:
        print(f"[!] Error opening serial port: {e}")
        sys.exit(1)

    reader_thread = threading.Thread(target=read_from_port, args=(ser,), daemon=True)
    reader_thread.start()

    try:
        while True:
            user_input = input()
            if user_input.lower() in ['quit', 'exit']:
                break
            
            # SlimeVR format: append only \n
            command = f"{user_input}\n"
            ser.write(command.encode('utf-8'))
            
    except KeyboardInterrupt:
        print("\n[*] Interrupted by user.")
    finally:
        if ser.is_open:
            ser.close()
        print("[*] Serial port closed. Goodbye.")

if __name__ == '__main__':
    main()