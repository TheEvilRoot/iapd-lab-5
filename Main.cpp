#include <iostream>
#include <unordered_map>
#include <functional>
#include <atomic>

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

struct Notification {
  HANDLE volume;
  HANDLE notification;
  char letter;

  bool operator==(const Notification& n) const {
    return n.volume == volume && n.notification == notification & n.letter == letter;
  }
};

DefaultMap<UINT, std::function<long(HWND, UINT, WPARAM, LPARAM)>> messageHandlers([](auto hwnd, auto uMsg, auto wParam, auto lParam) -> long { 
  //std::cout << "DefaultHandler :: Message 0x" << std::hex << uMsg << "\n";
  return DefWindowProc(hwnd, uMsg, wParam, lParam);
});

std::atomic<bool> allowMessageEvents{ true };
std::atomic<Key> lastKey;
HANDLE readyEvent;
std::vector<Notification> events;

Key readConsole(HANDLE stdInput) {
  INPUT_RECORD rec{};
  DWORD count{ 0 };
  while (true) {
    ReadConsoleInputA(stdInput, &rec, 1, &count);
    if (rec.EventType == KEY_EVENT) {
      auto& keyEvent = (KEY_EVENT_RECORD&)rec.Event;
      if (keyEvent.bKeyDown) {
        Key key{ keyEvent.uChar.AsciiChar, keyEvent.wVirtualKeyCode, keyEvent.wVirtualScanCode };
        lastKey = key;
        SetEvent(readyEvent);
        return key;
      }
    }
  }
  throw 0;
}

Key readConsoleThreadSafe(HANDLE stdInput) {
  ResetEvent(readyEvent);
  WaitForSingleObject(readyEvent, INFINITE);

  return lastKey.load();
}

auto unmaskVolumeLetters(DWORD mask) {
  std::string ret;
  for (char letter = 'A'; letter < 'Z'; letter++) {
    if ((mask >> (letter - 'A')) & 1) {
      ret += letter;
    }
  }
  return ret;
}

auto letterByVolumeHandle(HANDLE handle) {
  for (const auto &n : events) {
    if (n.volume == handle) {
      return n.letter;
    }
  }
  return '\0';
}

auto unregisterVolumeNotifications(HANDLE handle) {
  for (const auto &n : events) {
    if (n.volume == handle) {
      CloseHandle(n.volume);
      UnregisterDeviceNotification(n.notification);
      events.erase(std::find(events.begin(), events.end(), n));
      break;
    }
  }
}

auto unregisterVolumeNotifications(char letter) {
  for (const auto &n : events) {
    if (n.letter == letter) {
      CloseHandle(n.volume);
      UnregisterDeviceNotification(n.notification);
      events.erase(std::find(events.begin(), events.end(), n));
      break;
    }
  }
}

auto registerVolumeNotification(HWND window, HANDLE handle, char letter) {
  DEV_BROADCAST_HANDLE broadcast{};
  broadcast.dbch_size = sizeof(DEV_BROADCAST_HANDLE);
  broadcast.dbch_devicetype = DBT_DEVTYP_HANDLE;
  broadcast.dbch_handle = handle;
  events.push_back({ handle, RegisterDeviceNotification(window, &broadcast, 0), letter });
}

auto registerVolumeNotification(HWND window, char letter) {
  auto path = std::string("\\\\.\\") + std::string(1, letter) + std::string(":");
  auto handle = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_WRITE | FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    std::cerr << "Failed to open volume " << path << " :: " << GetLastError() << "\n";
    return;
  }
  registerVolumeNotification(window, handle, letter);
}

auto registerVolumesEvents(HWND window) {
  std::vector<Notification> events;
  for (auto letter : unmaskVolumeLetters(GetLogicalDrives())) {
    registerVolumeNotification(window, letter);
  }
}

void handleDeviceArrivalOrRemoval(HWND window, std::string arrivalOrRemoval, LPARAM lParam) {
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
      for (char letter : letters) {
        std::cout << std::string(1, letter) << " ";
        if (arrivalOrRemoval == "Removed") {
          unregisterVolumeNotifications(letter);
        } else {
          registerVolumeNotification(window, letter);
        }
      }
    }
    std::cout << "\n";
  }
  if (data->dbch_devicetype == DBT_DEVTYP_PORT) {
    DEV_BROADCAST_PORT_A* device = (DEV_BROADCAST_PORT_A*)lParam;
    std::cout << arrivalOrRemoval << " port device : " << std::string(device->dbcp_name) << "\n";
  }
}

long handleQueryRemove(LPARAM param) {
  if (!allowMessageEvents) return BROADCAST_QUERY_DENY;

  auto* data = (DEV_BROADCAST_HDR*)param;
  auto* handleDevice = (DEV_BROADCAST_HANDLE*)param;
  auto letter = letterByVolumeHandle(handleDevice->dbch_handle);

  std::cout << "Safe device removal request has arrived\n";
  std::cout << "Allow to remove device " << std::string(1, letter) << ":?\n";
  std::cout << "Use ENTER to allow, BACKSPACE to deny\n";

  auto answer = readConsoleThreadSafe(GetStdHandle(STD_INPUT_HANDLE));
  if (answer.key == 0xd && answer.scan == 0x1c) {
    unregisterVolumeNotifications(handleDevice->dbch_handle);
    std::cout << "Device remove is allowed\n";
    return TRUE;
  }
  if (answer.scan == 0x0e && answer.key == 0x08) {
    std::cout << "Device remove is denied\n";
    return BROADCAST_QUERY_DENY;
  }
}

void handleDeviceRemovalFailed(LPARAM lParam) {
  std::cout << "Device failed to remove!\n";
  DEV_BROADCAST_HDR* data = (DEV_BROADCAST_HDR*)lParam;
  std::cout << "Type " << data->dbch_devicetype << "\n";
}

void initMessageHandlers() {
  messageHandlers.set(0x219, [](auto hwnd, auto uMsg, auto wParam, auto lParam) -> long {
   // std::cout << "Device changed event: 0x" << std::hex << wParam << "\n";

    if (wParam == 0x8004) 
      handleDeviceArrivalOrRemoval(hwnd, "Removed", lParam);
    if (wParam == 0x8000)
      handleDeviceArrivalOrRemoval(hwnd, "Connected", lParam);
    if (wParam == 0x8001)
      return handleQueryRemove(lParam);
    if (wParam == 0x8002)
      handleDeviceRemovalFailed(lParam);

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

int readInt(HANDLE stdInput) {
  int value = 0;
  while (true) {
    auto k = readConsole(stdInput);
    if (k.key == 0xd && k.scan == 0x1c) {
      std::cout << "\n";
      return value;
    }
    if (k.ascii >= '0' && k.ascii <= '9') {
      value *= 10;
      value += k.ascii - '0';
      std::cout << std::string(1, k.ascii);
      if (value < 0) {
        std::cout << "\n";
        return -1;
      }
    }
  }
}

DWORD __stdcall consoleThreadHandler(LPVOID param) {
  auto context = *(Context*)param;
  while (true) {
    auto key = readConsole(context.stdInput);
    if (key.scan == 0x01 && key.key == 0x1b) { // ESC
      SendMessage(context.window, WM_CLOSE, 0, 0);
      return 0;
    }
    if (key.scan == 0x3b && key.key == 0x70) { // F1
      allowMessageEvents = false;
      std::cout << "List of volumes to remove: \n";
      for (int i = 0; i < events.size(); i++) {
        std::cout << "[" << i << "]" << "Device with volume " << events[i].letter << ": \n";
      }
      int value = 0;
      while ((value = readInt(context.stdInput)) < 0 || value >= events.size()) {
        std::cout << "Error: OutOfBound\n";
      }
      allowMessageEvents = true;
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

  readyEvent = CreateEventA(nullptr, true, false, nullptr);

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