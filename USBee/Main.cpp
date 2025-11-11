#include <windows.h>
#include <dbt.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devguid.h>
#include <initguid.h>
#include <usbiodef.h>
#include <string>
#include <vector>
#include <mutex>
#include <sstream>

#include <opencv2/opencv.hpp>

#include "ImGui/imgui.h"
#include "ImGui/imgui_impl_win32.h"
#include "ImGui/imgui_impl_opengl3.h"
#include <GL/gl.h>

#include <shellscalingapi.h>
#pragma comment(lib, "Shcore.lib")

#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")

#ifdef _DEBUG
#pragma comment(lib, "opencv_world4120d.lib")
#else
#pragma comment(lib, "opencv_world4120.lib")
#endif


extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

struct USBDevice {
    std::wstring devicePath;
    std::wstring deviceName;
    std::wstring deviceType;
    DEVINST devInst;
    bool isKeyboard;
    bool isMouse;
};

std::vector<DEVINST> allowed_devices;

// Global variables
std::mutex g_deviceMutex;
HDEVNOTIFY g_hDeviceNotify = NULL;
bool g_showPasswordDialog = false;
char g_passwordInput[256] = "";
const char* g_correctPassword = "999888999";
HWND g_mainWindow = NULL;
HWND g_popupWindow = NULL;
bool g_isRunning = true;
USBDevice g_currentDevice;
bool g_deviceBlocked = false;
HHOOK keyboardHook;

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) && g_showPasswordDialog) {
        KBDLLHOOKSTRUCT* p = (KBDLLHOOKSTRUCT*)lParam;
        // Block dangerous keys
        if ((p->vkCode == VK_TAB && (GetAsyncKeyState(VK_MENU) & 0x8000)) || // Alt+Tab
            (p->vkCode == VK_ESCAPE && (GetAsyncKeyState(VK_CONTROL) & 0x8000)) || // Ctrl+Esc
            (p->vkCode == VK_LWIN || p->vkCode == VK_RWIN)) // Windows key
        {
            return 1; // block
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

// Function prototypes
LRESULT CALLBACK PopupWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

std::string getDateTimeString() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    struct tm localTime;
    localtime_s(&localTime, &t);
    return std::format("{:04}-{:02}-{:02} {:02}-{:02}-{:02}",
        localTime.tm_year + 1900,
        localTime.tm_mon + 1,
        localTime.tm_mday,
        localTime.tm_hour,
        localTime.tm_min,
        localTime.tm_sec);
}

bool isAlreadyRunning() {
    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"Global\\USBeeSingleInstanceMutex");

    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Mutex sudah ada -> program lain dengan nama yang sama sudah jalan
        CloseHandle(hMutex);
        return true;
    }

    return false;
}

bool takePhoto() {
    cv::VideoCapture camera(0);

    // Pastikan kamera bisa dibuka
    if (!camera.isOpened()) {
        return false;
    }

    cv::Mat frame;
    camera >> frame;

    if (frame.empty()) {
        return false;
    }

    char tempPath[MAX_PATH];
    GetTempPathA(MAX_PATH, tempPath);

    std::string usbDir = std::string(tempPath) + "USBee";
    std::string file = usbDir + "\\" + getDateTimeString() + ".png";
    DWORD attrs = GetFileAttributesA(usbDir.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        if (CreateDirectoryA(usbDir.c_str(), NULL)) {
            std::cout << "[OK] Folder created: " << usbDir << std::endl;
        }
        else {
            std::cout << "[!] Failed to create folder. Error: " << GetLastError() << std::endl;
        }
    }
    else {
        std::cout << "[OK] Folder already exists: " << usbDir << std::endl;
    }

    

    if (!cv::imwrite(usbDir + "\\" + getDateTimeString() + ".png", frame)) {
        return false;
    }

    return true;
}

std::wstring GetDeviceInstanceId(const wchar_t* devicePath) {
    std::wstring path = devicePath;

    // Remove \\?\ prefix if present
    if (path.find(L"\\\\?\\") == 0) {
        path = path.substr(4);
    }

    // Replace # with \ for device instance ID format
    for (size_t i = 0; i < path.length(); i++) {
        if (path[i] == L'#') {
            path[i] = L'\\';
        }
    }

    // Remove GUID part at the end
    size_t lastBackslash = path.find_last_of(L'\\');
    if (lastBackslash != std::wstring::npos) {
        path = path.substr(0, lastBackslash);
    }

    return path;
}

std::wstring GetDeviceFriendlyName(DEVINST devInst) {
    wchar_t buffer[256] = { 0 };
    ULONG bufferLen = sizeof(buffer);

    if (CM_Get_DevNode_Registry_PropertyW(devInst, CM_DRP_FRIENDLYNAME,
        NULL, buffer, &bufferLen, 0) == CR_SUCCESS) {
        return std::wstring(buffer);
    }

    bufferLen = sizeof(buffer);
    if (CM_Get_DevNode_Registry_PropertyW(devInst, CM_DRP_DEVICEDESC,
        NULL, buffer, &bufferLen, 0) == CR_SUCCESS) {
        return std::wstring(buffer);
    }

    return L"Unknown Device";
}

std::wstring GetDeviceType(DEVINST devInst, bool& isKeyboard, bool& isMouse) {
    GUID classGuid;
    ULONG size = sizeof(GUID);
    isKeyboard = false;
    isMouse = false;

    if (CM_Get_DevNode_Registry_PropertyW(devInst, CM_DRP_CLASSGUID,
        NULL, &classGuid, &size, 0) == CR_SUCCESS) {

        if (IsEqualGUID(classGuid, GUID_DEVCLASS_KEYBOARD)) {
            isKeyboard = true;
            return L"USB Keyboard";
        }
        else if (IsEqualGUID(classGuid, GUID_DEVCLASS_MOUSE)) {
            isMouse = true;
            return L"USB Mouse";
        }
        else if (IsEqualGUID(classGuid, GUID_DEVCLASS_DISKDRIVE)) {
            return L"USB Storage Device";
        }
        else if (IsEqualGUID(classGuid, GUID_DEVCLASS_HIDCLASS)) {
            return L"USB HID Device (Possible Keyboard/Mouse)";
        }
        else if (IsEqualGUID(classGuid, GUID_DEVCLASS_USB)) {
            return L"Generic USB Device";
        }
        else if (IsEqualGUID(classGuid, GUID_DEVCLASS_MEDIA)) {
            return L"USB Media Device";
        }
    }

    return L"Unknown USB Device";
}
bool IsSkipDevice(DEVINST devInst) {
    GUID classGuid;
    ULONG size = sizeof(GUID);

    if (CM_Get_DevNode_Registry_PropertyW(devInst, CM_DRP_CLASSGUID,
        NULL, &classGuid, &size, 0) == CR_SUCCESS) {
        if (IsEqualGUID(classGuid, GUID_DEVCLASS_DISKDRIVE)) {
            return true;
        }
        else if (IsEqualGUID(classGuid, GUID_DEVCLASS_MEDIA)) {
            return true;
        }
    }

    return false;
}


bool DisableDevice(DEVINST devInst) {
    CONFIGRET ret = CM_Disable_DevNode(devInst, 0);
    return (ret == CR_SUCCESS);
}

bool EnableDevice(DEVINST devInst) {
    CONFIGRET ret = CM_Enable_DevNode(devInst, 0);
    allowed_devices.push_back(devInst);
    return (ret == CR_SUCCESS);
}

void ShowPopupWindow() {
    if (g_popupWindow != NULL && IsWindow(g_popupWindow)) {
        SetForegroundWindow(g_popupWindow);
        return;
    }

    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW), CS_CLASSDC, PopupWndProc, 0L, 0L,
                      GetModuleHandle(NULL), NULL, NULL, NULL, NULL,
                      L"USBProtectionPopup", NULL };
    RegisterClassExW(&wc);

    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    int windowWidth = 650;
    int windowHeight = 450;

    g_popupWindow = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        wc.lpszClassName,
        L"USB Security Alert",
        WS_POPUP | WS_VISIBLE,
        0,
        0,
        screenWidth, screenHeight,
        NULL, NULL, wc.hInstance, NULL
    );
        
    ShowWindow(g_popupWindow, SW_SHOW);
    SetForegroundWindow(g_popupWindow);
    SetFocus(g_popupWindow);
    UpdateWindow(g_popupWindow);
}

void ClosePopupWindow() {
    if (g_popupWindow != NULL) {
        DestroyWindow(g_popupWindow);
        g_popupWindow = NULL;
    }
}

LRESULT CALLBACK BackgroundWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_DEVICECHANGE: {
        if (wParam == DBT_DEVICEARRIVAL) {
            DEV_BROADCAST_HDR* pHdr = (DEV_BROADCAST_HDR*)lParam;

            if (pHdr->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE) {
                DEV_BROADCAST_DEVICEINTERFACE_W* pDevInf =
                    (DEV_BROADCAST_DEVICEINTERFACE_W*)lParam;

                std::wstring instanceId = GetDeviceInstanceId(pDevInf->dbcc_name);

                DEVINST devInst;
                CONFIGRET ret = CM_Locate_DevNodeW(&devInst,
                    (DEVINSTID_W)instanceId.c_str(),
                    CM_LOCATE_DEVNODE_NORMAL);

                if (ret == CR_SUCCESS) {
                    USBDevice device;
                    device.devicePath = pDevInf->dbcc_name;
                    device.devInst = devInst;

                    bool isKeyboard = false;
                    bool isMouse = false;
                    device.deviceName = GetDeviceFriendlyName(devInst);
                    device.deviceType = GetDeviceType(devInst, isKeyboard, isMouse);
                    device.isKeyboard = isKeyboard;
                    device.isMouse = isMouse;

                    bool Skip = IsSkipDevice(devInst);
                    bool alreadyAllowed = (std::find(allowed_devices.begin(), allowed_devices.end(), devInst) != allowed_devices.end());
                    
                    if (!Skip && !alreadyAllowed) {
                        bool disabled = DisableDevice(devInst);

                        std::lock_guard<std::mutex> lock(g_deviceMutex);
                        g_currentDevice = device;
                        g_deviceBlocked = disabled;
                        g_showPasswordDialog = true;

                        // Show popup window
                        ShowPopupWindow();
                        std::thread(takePhoto).detach();
                    }
                }
            }
        }
        return TRUE;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK PopupWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_CLOSE:
        // Don't enable device on close
        if (g_deviceBlocked) {
            MessageBoxW(hwnd, L"Device remains blocked for security.",
                L"Device Blocked", MB_ICONWARNING);
        }
        ClosePopupWindow();
        g_showPasswordDialog = false;
        memset(g_passwordInput, 0, sizeof(g_passwordInput));
        return 0;

    case WM_DESTROY:
        g_popupWindow = NULL;
        return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

bool InitOpenGL(HWND hwnd, HDC* hdc, HGLRC* hrc) {
    *hdc = GetDC(hwnd);

    PIXELFORMATDESCRIPTOR pfd = {
        sizeof(PIXELFORMATDESCRIPTOR), 1,
        PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
        PFD_TYPE_RGBA, 32, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 24, 8, 0,
        PFD_MAIN_PLANE, 0, 0, 0, 0
    };

    int pixelFormat = ChoosePixelFormat(*hdc, &pfd);
    if (!pixelFormat) return false;

    if (!SetPixelFormat(*hdc, pixelFormat, &pfd)) return false;

    *hrc = wglCreateContext(*hdc);
    if (!*hrc) return false;

    wglMakeCurrent(*hdc, *hrc);
    return true;
}

bool IsAdministrator() {
    BOOL isAdmin = FALSE;
    PSID adminGroup = NULL;
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;

    if (AllocateAndInitializeSid(&ntAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID,
        DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &adminGroup)) {
        CheckTokenMembership(NULL, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }

    return isAdmin;
}

std::string WStringToString(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, NULL, 0, NULL, NULL);
    std::string str(size - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &str[0], size, NULL, NULL);
    return str;
}

//int main() {
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    if (isAlreadyRunning()) {
        MessageBoxA(NULL, "This application is already running.", "Application Already Running.", MB_ICONERROR);
        return 0;
    }

    if (!IsAdministrator()) {
        MessageBoxW(NULL,
            L"This application requires Administrator privileges to control USB devices.\n\n"
            L"Please right-click and select 'Run as Administrator'.",
            L"Administrator Rights Required",
            MB_ICONERROR);
        return 1;
    }

    SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);

    keyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, NULL, 0);

    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW), CS_CLASSDC, BackgroundWndProc, 0L, 0L,
                      GetModuleHandle(NULL), NULL, NULL, NULL, NULL,
                      L"USBProtectionBackground", NULL };
    RegisterClassExW(&wc);

    g_mainWindow = CreateWindowW(wc.lpszClassName, L"USB Protection Service",
        WS_OVERLAPPEDWINDOW, 0, 0, 1, 1,
        NULL, NULL, wc.hInstance, NULL);

    if (!g_mainWindow) {
        MessageBoxW(NULL, L"Failed to create background window.", L"Error", MB_ICONERROR);
        return 1;
    }

    DEV_BROADCAST_DEVICEINTERFACE_W notificationFilter = {};
    notificationFilter.dbcc_size = sizeof(DEV_BROADCAST_DEVICEINTERFACE_W);
    notificationFilter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    notificationFilter.dbcc_classguid = GUID_DEVINTERFACE_USB_DEVICE;

    g_hDeviceNotify = RegisterDeviceNotificationW(g_mainWindow, &notificationFilter,
        DEVICE_NOTIFY_WINDOW_HANDLE);

    if (!g_hDeviceNotify) {
        MessageBoxW(NULL, L"Failed to register for device notifications.", L"Error", MB_ICONERROR);
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();

    // Startup notification
#ifdef _DEBUG
    MessageBoxW(NULL,
        L"USB Protection Monitor is now active.\n\n"
        L"✓ Running in background\n"
        L"✓ Monitoring all USB ports\n"
        L"✓ Will prompt for password on USB detection\n\n"
        L"Protected",
        L"USB Protection Active",
        MB_ICONINFORMATION);
#endif

    HDC hdc = NULL;
    HGLRC hrc = NULL;

    MSG msg;
    ZeroMemory(&msg, sizeof(msg));

    while (g_isRunning) {
        while (PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);

            if (msg.message == WM_QUIT) {
                g_isRunning = false;
            }
        }

        if (!g_isRunning) break;

        if (g_popupWindow != NULL && IsWindow(g_popupWindow)) {
            if (hdc == NULL) {
                if (!InitOpenGL(g_popupWindow, &hdc, &hrc)) {
                    MessageBoxW(NULL, L"Failed to initialize OpenGL.", L"Error", MB_ICONERROR);
                    g_isRunning = false;
                    break;
                }
                ImGui_ImplWin32_Init(g_popupWindow);
                ImGui_ImplOpenGL3_Init("#version 130");
            }

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            ImVec2 center = ImGui::GetMainViewport()->GetCenter();

            ImVec2 window_size(600, 400);

            ImVec2 window_pos(center.x - window_size.x * 0.5f, center.y - window_size.y * 0.5f);

            ImGui::SetNextWindowPos(window_pos, ImGuiCond_Always);
            ImGui::SetNextWindowSize(window_size);
            ImGui::Begin("USB Security Alert", NULL,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

            ImGui::SetWindowFontScale(1.3f);

            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.2f, 0.2f, 1.0f));
            ImGui::Text("  WARNING: USB DEVICE DETECTED!");
            ImGui::PopStyleColor();

            ImGui::Separator();
            ImGui::Spacing();

            ImGui::SetWindowFontScale(1.1f);

            std::string deviceName = WStringToString(g_currentDevice.deviceName);
            std::string deviceType = WStringToString(g_currentDevice.deviceType);

            ImGui::Text("Device Name:");
            ImGui::SameLine(180);
            ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.0f, 1.0f), "%s", deviceName.c_str());

            ImGui::Text("Device Type:");
            ImGui::SameLine(180);
            ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.0f, 1.0f), "%s", deviceType.c_str());

            ImGui::Text("Status:");
            ImGui::SameLine(180);
            if (g_deviceBlocked) {
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "BLOCKED");
            }
            else {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "ACTIVE (Admin rights needed to block)");
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.0f, 1.0f));
            ImGui::TextWrapped("A USB device has been connected to your computer.");
            ImGui::PopStyleColor();

            ImGui::Spacing();
            ImGui::TextWrapped("This could be:");
            ImGui::BulletText("A legitimate USB drive or peripheral");
            ImGui::BulletText("A fake keyboard (USB Rubber Ducky)");
            ImGui::BulletText("A malicious device (BadUSB attack)");
            ImGui::BulletText("An unauthorized data exfiltration device");

            if (g_currentDevice.isKeyboard) {
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
                ImGui::TextWrapped("CAUTION: This device identifies as a KEYBOARD!");
                ImGui::PopStyleColor();
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::Text("Enter Password to Allow:");
            ImGui::SetNextItemWidth(450);
            bool enterPressed = ImGui::InputText("##password", g_passwordInput, sizeof(g_passwordInput),
                ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);

            ImGui::Spacing();
            ImGui::Spacing();

            bool allowClicked = ImGui::Button("Allow Device", ImVec2(280, 50)) || enterPressed;

            ImGui::SameLine();

            bool blockClicked = ImGui::Button("Keep Blocked", ImVec2(280, 50));

            if (allowClicked) {
                if (strcmp(g_passwordInput, g_correctPassword) == 0) {
                    if (g_deviceBlocked) {
                        if (EnableDevice(g_currentDevice.devInst)) {
                            MessageBoxW(g_popupWindow, L"Device access granted!", L"Success", MB_ICONINFORMATION);
                        }
                        else {
                            MessageBoxW(g_popupWindow, L"Failed to enable device. Check permissions.", L"Error", MB_ICONERROR);
                        }
                    }
                    else {
                        MessageBoxW(g_popupWindow, L"Device is already active.", L"Info", MB_ICONINFORMATION);
                    }

                    if (hdc && hrc) {
                        HWND tempHwnd = g_popupWindow;

                        ClosePopupWindow();
                        g_showPasswordDialog = false;
                        memset(g_passwordInput, 0, sizeof(g_passwordInput));

                        ImGui_ImplOpenGL3_Shutdown();
                        ImGui_ImplWin32_Shutdown();
                        wglMakeCurrent(NULL, NULL);
                        wglDeleteContext(hrc);
                        ReleaseDC(tempHwnd, hdc);
                        hdc = NULL;
                        hrc = NULL;

                        continue;
                    }
                }
                else {
                    MessageBoxW(g_popupWindow, L"Incorrect password! Device remains blocked.", L"Access Denied", MB_ICONERROR);
                    memset(g_passwordInput, 0, sizeof(g_passwordInput));
                }
            }

            if (blockClicked) {
                MessageBoxW(g_popupWindow, L"Device will remain blocked.", L"Device Blocked", MB_ICONWARNING);

                if (hdc && hrc) {
                    HWND tempHwnd = g_popupWindow;

                    ClosePopupWindow();
                    g_showPasswordDialog = false;
                    memset(g_passwordInput, 0, sizeof(g_passwordInput));

                    ImGui_ImplOpenGL3_Shutdown();
                    ImGui_ImplWin32_Shutdown();
                    wglMakeCurrent(NULL, NULL);
                    wglDeleteContext(hrc);
                    ReleaseDC(tempHwnd, hdc);
                    hdc = NULL;
                    hrc = NULL;

                    continue;
                }
            }

            ImGui::End();

            ImGui::Render();
            glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
            glClearColor(0.1f, 0.1f, 0.12f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

            SwapBuffers(hdc);
        }

        Sleep(16);
    }

    if (hdc && hrc) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplWin32_Shutdown();
        wglMakeCurrent(NULL, NULL);
        wglDeleteContext(hrc);
        ReleaseDC(g_popupWindow, hdc);
    }

    ImGui::DestroyContext();

    if (g_hDeviceNotify) {
        UnregisterDeviceNotification(g_hDeviceNotify);
    }

    return 0;
}