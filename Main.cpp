#include <iostream>
#include <unordered_map>
#include <functional>

#include <Windows.h>
#include <dbt.h>

EXTERN_C const GUID DECLSPEC_SELECTANY GUID_DEVINTERFACE_USB_DEVICE = { 0xA5DCBF10L, 0x6530, 0x11D2, { 0x90, 0x1F,  0x00,  0xC0,  0x4F,  0xB9,  0x51,  0xED } };

template<typename K, typename V>
class DefaultMap {
private:
  std::unordered_map<K, V> map;
  V defaultValue;

public:
  DefaultMap(const V& defaultValue): defaultValue { defaultValue  } { }

  V& operator[](const K& key) {
    if (map.count(key) == 0) {
      return defaultValue;
    }
    return map[key];
  }

  void set(const K& key, const V& value) {
    map.insert({ key, value });
  }
};

struct Key {
  char ascii;
  WORD key;
  WORD scan;
};

struct Context {
  HWND window;
  HANDLE stdInput;
};

DefaultMap<UINT, std::function<long(HWND, UINT, WPARAM, LPARAM)>> messageHandlers([](auto hwnd, auto uMsg, auto wParam, auto lParam) -> long { 
  std::cout << "DefaultHandler :: Message 0x" << std::hex << uMsg << "\n";
  return DefWindowProc(hwnd, uMsg, wParam, lParam);
});

auto unmaskVolumeLetters(DWORD mask) {
  std::string ret;
  for (char letter = 'A'; letter < 'Z'; letter++) {
    if ((mask >> (letter - 'A')) & 1) {
      ret += letter;
    }
  }
  return ret;
}

void handleDeviceArrivalOrRemoval(std::string arrivalOrRemoval, LPARAM lParam) {
  DEV_BROADCAST_HDR* data = (DEV_BROADCAST_HDR*)lParam;
  if (data->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE) {
    DEV_BROADCAST_DEVICEINTERFACE_A* device = (DEV_BROADCAST_DEVICEINTERFACE_A*)lParam;
    auto name = std::string(device->dbcc_name);
    if (name.size() <= 1) {
      std::cout << arrivalOrRemoval << " HID device\n";
    } else {
      std::cout << arrivalOrRemoval << " HID device : " << "\n";
    }
  }
  if (data->dbch_devicetype == DBT_DEVTYP_VOLUME) {
    DEV_BROADCAST_VOLUME* device = (DEV_BROADCAST_VOLUME*)lParam;
    if (device->dbcv_flags & DBTF_MEDIA) {
      std::cout << " storage media device : ";
    } else if (device->dbcv_flags & DBTF_NET) {
      std::cout << arrivalOrRemoval << " storage network device : ";
    } else {
      std::cout << arrivalOrRemoval << " storage device : ";
    }
    auto letters = unmaskVolumeLetters(device->dbcv_unitmask);
    if (letters.empty()) {
      std::cout << arrivalOrRemoval << "No volumes were mounted";
    } else {
      for (char letter : letters)
        std::cout << std::string(1, letter) << " ";
    }
    std::cout << "\n";
  }
  if (data->dbch_devicetype == DBT_DEVTYP_PORT) {
    DEV_BROADCAST_PORT_A* device = (DEV_BROADCAST_PORT_A*)lParam;
    std::cout << arrivalOrRemoval << " port device : " << std::string(device->dbcp_name) << "\n";
  }
}

void initMessageHandlers() {
  messageHandlers.set(0x219, [](auto hwnd, auto uMsg, auto wParam, auto lParam) -> long {
    //std::cout << "Device changed event: 0x" << std::hex << wParam << "\n";

    if (wParam == 0x8004) 
      handleDeviceArrivalOrRemoval("Removed", lParam);
    if (wParam == 0x8000)
      handleDeviceArrivalOrRemoval("Connected", lParam);

    return DefWindowProc(hwnd, uMsg, wParam, lParam);
  });
  messageHandlers.set(WM_CLOSE, [](auto window, auto, auto, auto) -> long {
    DestroyWindow(window);
    return 0;
  });
  messageHandlers.set(WM_DESTROY, [](auto window, auto, auto, auto) -> long {
    PostQuitMessage(0);
    return 0;
  });
}

long __stdcall windowHandler(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
  return messageHandlers[uMsg](hwnd, uMsg, wParam, lParam);
}

Key readConsole(HANDLE stdInput) {
  INPUT_RECORD rec{};
  DWORD count{ 0 };
  while (true) {
    ReadConsoleInputA(stdInput, &rec, 1, &count);
    if (rec.EventType == KEY_EVENT) {
      auto& keyEvent = (KEY_EVENT_RECORD&)rec.Event;
      if (keyEvent.bKeyDown) {
        return Key{ keyEvent.uChar.AsciiChar, keyEvent.wVirtualKeyCode, keyEvent.wVirtualScanCode };
      }
    }
  }
  throw 0;
}

DWORD __stdcall consoleThreadHandler(LPVOID param) {
  auto context = *(Context*)param;
  while (true) {
    auto key = readConsole(context.stdInput);
    if (key.scan == 0x01 && key.key == 0x1b) {
      SendMessage(context.window, WM_CLOSE, 0, 0);
      return 0;
    }
    if (key.scan == 0x3b && key.key == 0x70) {

    }
  }
  return 0;
}

void printPrompt() {
  std::cout << "Welcome to something for monitoring usb devices\n";
  std::cout << "We're using WinAPI window but i hope you can't see it\n";
  std::cout << "There's some hot keys to with with ...that thingy...:\n";
  std::cout << "\tUse ESC to exit the program\n";
  std::cout << "\tUse F1 to request safe ejection for usb device\n";
}

auto registerVolumesEvents(HWND window) {
  std::vector<HANDLE> events;
  auto prefix = std::string("\\\\.\\");
  for (auto letter : unmaskVolumeLetters(GetLogicalDrives())) {
    auto path = prefix + std::string(1, letter) + std::string(":\\");
    auto handle = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    DEV_BROADCAST_HANDLE broadcast{};
    broadcast.dbch_size = sizeof(DEV_BROADCAST_HANDLE);
    broadcast.dbch_devicetype = DBT_DEVTYP_HANDLE;
    broadcast.dbch_handle = handle;
    events.push_back(RegisterDeviceNotification(window, &broadcast, 0));
  }
  return events;
}

int main() {
  auto instance = GetModuleHandle(nullptr);

  auto stdInput = GetStdHandle(STD_INPUT_HANDLE);
  DWORD mode{ 0 };
  GetConsoleMode(stdInput, &mode);
  SetConsoleMode(stdInput, mode & ~ENABLE_ECHO_INPUT & ~ENABLE_QUICK_EDIT_MODE);

  const auto* windowClassName = L"WindowClass";
  WNDCLASS wc{ };
  wc.lpfnWndProc = &windowHandler;
  wc.hInstance = instance;
  wc.lpszClassName = windowClassName;
  RegisterClass(&wc);

  auto window = CreateWindowEx(0, windowClassName, L"Dummy", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 0, 0, nullptr, nullptr, instance, nullptr);

  DEV_BROADCAST_DEVICEINTERFACE broadcast{};
  broadcast.dbcc_size = sizeof(DEV_BROADCAST_DEVICEINTERFACE);
  broadcast.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
  broadcast.dbcc_classguid = GUID_DEVINTERFACE_USB_DEVICE;

  auto registerResult = RegisterDeviceNotification(window, &broadcast, DEVICE_NOTIFY_WINDOW_HANDLE);
  std::cout << "RegisterDeviceNotification: " << registerResult << "\n";
  if (registerResult == nullptr) {
    std::cout << "Error : " << GetLastError() << "\n";
  }

  registerVolumesEvents(window);

  printPrompt();
  initMessageHandlers();
  UpdateWindow(window);

  Context context{ window, stdInput };
  auto consoleThread = CreateThread(nullptr, 0, &consoleThreadHandler, &context, 0, nullptr);
  if (consoleThread == INVALID_HANDLE_VALUE) {
    std::cerr << "failed to create thread\n";
    DestroyWindow(window);
    return 1;
  }

  MSG message{};
  while (GetMessage(&message, window, 0, 0))  {
    TranslateMessage(&message);
    DispatchMessage(&message);
  }
  
  TerminateThread(consoleThread, 0);
  WaitForSingleObject(consoleThread, INFINITE);
  CloseHandle(consoleThread);
}