# USBee - Smart USB Protection for Windows

USBee is a Windows security application designed to prevent unauthorized or potentially dangerous USB access.  
Whenever a new USB device is inserted into the computer, USBee automatically disables the device and prompts the user for a password before allowing the device to operate.

---

## Features

- **Real-Time USB Detection**  
  USBee uses Windows API to monitor every USB device connected to the system.

- **Automatic Prevention**  
  When a new USB device is detected, USBee immediately disables it to prevent any unauthorized access or execution before verification.

- **Password Prompt (ImGui GUI)**  
  When a USB device is detected, a prompt will appear using ImGui asking for a password.  
  - If the password is **correct**, the USB device will be **enabled**.  
  - If the password is **incorrect** or the application is closed, the device will remain **disabled**.

- **Manual USB Monitoring**  
  If a user tries to manually enable the USB device (e.g., via Device Manager), USBee will detect the change, disable the device again, and show the password prompt.

- **Automatic Camera Capture During Prompt**  
  Every time the password prompt appears, USBee will automatically take a photo using the camera and save it to:
  %temp%/USBee/
  This feature helps identify who attempted to insert the USB device.
---

## Example Use Cases

- Prevent malware from USB drives from executing automatically.  
- Protect sensitive systems in offices or labs where USB access should be controlled.  
- Track and monitor unauthorized USB insertion attempts.

---

## Installation

1. Download the USBee.
2. Run the USBee as Administrator.  
3. USBee will start automatically and run in the background.

---

## Usage

1. Insert a USB device.  
2. The USB device will be disabled immediately.  
3. Enter the password in the prompt to enable the device.  
4. If the password is incorrect, the device will remain disabled, and a photo will be captured.

---

## License

This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for details.
