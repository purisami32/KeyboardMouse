#include <windows.h>
#include <shellapi.h>
#include <chrono>
#include <cmath>
#include <atomic>
#include "framework.h"
#include "KeyboardMouse.h"

// 定数定義
constexpr UINT_PTR TIMER_ID_KEYCHECK = 1;
constexpr UINT     ID_TRAY_ICON = 1;
constexpr UINT     ID_MENU_EXIT = 1001;
constexpr UINT     WM_NOTIFYICON = WM_USER + 1;

// 移動速度に関する設定
constexpr double SPEED_INCHES_PER_SEC = 3.0;
constexpr double SHIFT_MULTI = 4;
constexpr double DIAGONAL_SCALE = 0.7071067811865475; // 1 / sqrt(2)

// 四隅ジャンプ時の内側マージン比率（0.25 = 画面端から25%内側の位置）
constexpr double MARGIN_RATIO = 0.25;

// グローバル変数
HINSTANCE hInst = nullptr;
NOTIFYICONDATA nid = { 0 };
HHOOK hLowLevelKeyboardHook = nullptr;

// フラグ状態（アトミック変数でスレッド安全性を担保）
std::atomic<bool> g_isKeyboardMouseEnabled{ true };
std::atomic<bool> g_isTrayIconEnabled{ true };

std::atomic<bool> g_isLeftKeyDown{ false };
std::atomic<bool> g_isRightKeyDown{ false };
std::atomic<bool> g_isUpKeyDown{ false };
std::atomic<bool> g_isDownKeyDown{ false };
std::atomic<bool> g_isShiftKeyDown{ false };

// マウスボタンの押しっぱなし状態を保持（キーリピート対策）
std::atomic<bool> g_isMouseLeftDown{ false };
std::atomic<bool> g_isMouseMiddleDown{ false };
std::atomic<bool> g_isMouseRightDown{ false };

// 時刻管理
static auto g_lastTime = std::chrono::high_resolution_clock::now();

// 前方宣言
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK LowLevelKeyboardProc(int, WPARAM, LPARAM);
void ShowContextMenu(HWND);
void UpdateTrayIcon(HWND);
void ShowNotification(HWND hWnd, bool isEnabled);
void MouseProc(HWND);
void SendMouseClick(DWORD flags);
void SendMouseWheel(int scrollAmount);
void MoveCursorToCorner(int corner);

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR lpCmdLine,
    _In_ int nCmdShow)
{
    hInst = hInstance;

    // 二重起動防止
    HANDLE hMutex = CreateMutex(NULL, FALSE, L"KEYBOARD_MOUSE_MUTEX");
    if (hMutex == NULL || GetLastError() == ERROR_ALREADY_EXISTS) {
        return 0;
    }

    WNDCLASSEX wc = { sizeof(WNDCLASSEX) };
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = TEXT("KEYBOARD_MOUSE_CLASS");

    if (!RegisterClassEx(&wc)) {
        return 0;
    }

    // 不可視ウィンドウの作成
    HWND hWnd = CreateWindowEx(0, wc.lpszClassName, TEXT(""), 0, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);
    if (!hWnd) {
        return 0;
    }

    // 低レベルキーボードフックの設定
    hLowLevelKeyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, hInstance, 0);
    if (hLowLevelKeyboardHook == nullptr) {
        return 0;
    }

    // メッセージループ
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (hLowLevelKeyboardHook) {
        UnhookWindowsHookEx(hLowLevelKeyboardHook);
    }

    if (hMutex) {
        CloseHandle(hMutex);
    }

    return static_cast<int>(msg.wParam);
}

void ShowContextMenu(HWND hWnd) {
    POINT pt;
    GetCursorPos(&pt);

    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    AppendMenu(hMenu, MF_STRING, ID_MENU_EXIT, TEXT("終了(&X)"));

    SetForegroundWindow(hWnd);
    int clickedID = TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_RETURNCMD, pt.x, pt.y, 0, hWnd, NULL);
    DestroyMenu(hMenu);

    if (clickedID == ID_MENU_EXIT) {
        DestroyWindow(hWnd);
    }
}

// OSの通知トレイからバルーン（トースト）通知を表示する関数
void ShowNotification(HWND hWnd, bool isEnabled) {
    NOTIFYICONDATAW nidNotify = { 0 };
    nidNotify.cbSize = sizeof(NOTIFYICONDATAW);
    nidNotify.hWnd = hWnd;
    nidNotify.uID = ID_TRAY_ICON;
    nidNotify.uFlags = NIF_INFO;
    nidNotify.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;

    lstrcpyW(nidNotify.szInfoTitle, L"キーボードマウス");
    if (isEnabled) {
        lstrcpyW(nidNotify.szInfo, L"機能を ON にしました");
    }
    else {
        lstrcpyW(nidNotify.szInfo, L"機能を OFF にしました");
    }

    Shell_NotifyIconW(NIM_MODIFY, &nidNotify);
}

void UpdateTrayIcon(HWND hWnd) {
    bool isEnabled = g_isKeyboardMouseEnabled.load();
    if (isEnabled == g_isTrayIconEnabled.load()) return;

    NOTIFYICONDATAW nidUpdate = { 0 };
    nidUpdate.cbSize = sizeof(NOTIFYICONDATAW);
    nidUpdate.hWnd = hWnd;
    nidUpdate.uID = ID_TRAY_ICON;
    nidUpdate.uFlags = NIF_ICON;

    int iconId = isEnabled ? IDI_ENABLE : IDI_DISABLE;
    nidUpdate.hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(iconId));

    Shell_NotifyIconW(NIM_MODIFY, &nidUpdate);
    g_isTrayIconEnabled.store(isEnabled);

    // アイコンの切り替えと同時に通知を表示
    ShowNotification(hWnd, isEnabled);
}

// 画面の内側4箇所へカーソルを移動する処理
// 0: 左上内側, 1: 左下内側, 2: 右上内側, 3: 右下内側
void MoveCursorToCorner(int corner) {
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);

    // 画面端から内側に寄せるピクセル数を計算
    int marginX = static_cast<int>(screenWidth * MARGIN_RATIO);
    int marginY = static_cast<int>(screenHeight * MARGIN_RATIO);

    int x = 0;
    int y = 0;

    switch (corner) {
    case 0: // 左上（内側）
        x = marginX;
        y = marginY;
        break;
    case 1: // 左下（内側）
        x = marginX;
        y = screenHeight - marginY;
        break;
    case 2: // 右上（内側）
        x = screenWidth - marginX;
        y = marginY;
        break;
    case 3: // 右下（内側）
        x = screenWidth - marginX;
        y = screenHeight - marginY;
        break;
    }

    SetCursorPos(x, y);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        // タスクトレイアイコン登録
        ZeroMemory(&nid, sizeof(nid));
        nid.cbSize = sizeof(nid);
        nid.hWnd = hWnd;
        nid.uID = ID_TRAY_ICON;
        nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        nid.uCallbackMessage = WM_NOTIFYICON;
        nid.hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_ENABLE));
        lstrcpy(nid.szTip, TEXT("キーボードマウス"));
        Shell_NotifyIcon(NIM_ADD, &nid);

        // 8ms タイマー (約125FPS)
        if (SetTimer(hWnd, TIMER_ID_KEYCHECK, 8, NULL) == 0) {
            MessageBox(hWnd, L"タイマーの作成に失敗しました。", L"エラー", MB_OK | MB_ICONERROR);
            return -1;
        }
        break;

    case WM_TIMER:
        if (wParam == TIMER_ID_KEYCHECK) {
            MouseProc(hWnd);
        }
        break;

    case WM_NOTIFYICON:
        if (lParam == WM_RBUTTONUP || lParam == WM_LBUTTONUP) {
            ShowContextMenu(hWnd);
        }
        break;

    case WM_DESTROY:
        KillTimer(hWnd, TIMER_ID_KEYCHECK);
        Shell_NotifyIcon(NIM_DELETE, &nid);
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        auto* pkbd = reinterpret_cast<LPKBDLLHOOKSTRUCT>(lParam);
        bool isKeyDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        bool isKeyUp = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);

        // キー押下状態の保持（トグル切り替えのチャタリング防止用）
        static bool isToggleTriggered = false;

        // Shiftキー単体の状態更新（※ここでは判定のためにフラグを更新するのみでブロックはしない）
        if (pkbd->vkCode == VK_LSHIFT || pkbd->vkCode == VK_RSHIFT || pkbd->vkCode == VK_SHIFT) {
            if (isKeyDown) g_isShiftKeyDown.store(true);
            else if (isKeyUp) g_isShiftKeyDown.store(false);
        }

        // 修飾キー（Shift / Ctrl / Win）のリアルタイム物理状態の判定
        bool isShiftDown = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0 || g_isShiftKeyDown.load();
        bool isCtrlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 ||
            (pkbd->vkCode == VK_LCONTROL || pkbd->vkCode == VK_RCONTROL);
        bool isWinDown = (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 || (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;

        // --- 1. 機能のON/OFF切り替え (Shift + Ctrl 同時押し) ---
        if (isShiftDown && isCtrlDown) {
            // Shift + Ctrl 押下時でも V, C, F キーは透過させる
            if (pkbd->vkCode == 'V' || pkbd->vkCode == 'C' || pkbd->vkCode == 'F') {
                return CallNextHookEx(hLowLevelKeyboardHook, nCode, wParam, lParam);
            }

            if (isKeyDown && !isToggleTriggered) {
                isToggleTriggered = true; // キーが離されるまで再発火を防止
                bool nextState = !g_isKeyboardMouseEnabled.load();
                g_isKeyboardMouseEnabled.store(nextState);

                if (!nextState) {
                    g_isLeftKeyDown.store(false);
                    g_isRightKeyDown.store(false);
                    g_isUpKeyDown.store(false);
                    g_isDownKeyDown.store(false);
                    g_isShiftKeyDown.store(false);

                    // 機能OFF時は押下状態を強制解除
                    if (g_isMouseLeftDown.exchange(false)) SendMouseClick(MOUSEEVENTF_LEFTUP);
                    if (g_isMouseMiddleDown.exchange(false)) SendMouseClick(MOUSEEVENTF_MIDDLEDOWN);
                    if (g_isMouseRightDown.exchange(false)) SendMouseClick(MOUSEEVENTF_RIGHTUP);
                }
            }
            // Shift / Ctrl キー自体の押し離しでブロックせず透過させる
            if (pkbd->vkCode == VK_LSHIFT || pkbd->vkCode == VK_RSHIFT || pkbd->vkCode == VK_SHIFT ||
                pkbd->vkCode == VK_LCONTROL || pkbd->vkCode == VK_RCONTROL || pkbd->vkCode == VK_CONTROL) {
                return CallNextHookEx(hLowLevelKeyboardHook, nCode, wParam, lParam);
            }
            if (isKeyDown) return 1;
        }
        else {
            if (isKeyUp) {
                isToggleTriggered = false;
            }
        }

        // --- 2. 各種組み合わせの透過判定 ---

        // Shift キー単体または Ctrl キー単体の場合は常に透過させる
        if (pkbd->vkCode == VK_LSHIFT || pkbd->vkCode == VK_RSHIFT || pkbd->vkCode == VK_SHIFT ||
            pkbd->vkCode == VK_LCONTROL || pkbd->vkCode == VK_RCONTROL || pkbd->vkCode == VK_CONTROL) {
            return CallNextHookEx(hLowLevelKeyboardHook, nCode, wParam, lParam);
        }

        // [透過処理1] Shift + 矢印キー の透過
        if (isShiftDown && (pkbd->vkCode == VK_LEFT || pkbd->vkCode == VK_RIGHT ||
            pkbd->vkCode == VK_UP || pkbd->vkCode == VK_DOWN)) {
            return CallNextHookEx(hLowLevelKeyboardHook, nCode, wParam, lParam);
        }

        // [透過処理2] Shift + Win + S の透過
        if (isShiftDown && isWinDown && pkbd->vkCode == 'S') {
            return CallNextHookEx(hLowLevelKeyboardHook, nCode, wParam, lParam);
        }

        // [透過処理3] Ctrl キー単体組み合わせ（Ctrl + V, C, F）の透過判定
        if (isCtrlDown && !isShiftDown) {
            if (pkbd->vkCode == 'V' || pkbd->vkCode == 'C' || pkbd->vkCode == 'F') {
                return CallNextHookEx(hLowLevelKeyboardHook, nCode, wParam, lParam);
            }
        }

        // --- 3. キーボードマウス機能（ON時のみ動作） ---
        if (g_isKeyboardMouseEnabled.load()) {
            if (isKeyDown) {
                switch (pkbd->vkCode) {
                case 'J': g_isLeftKeyDown.store(true); return 1;
                case 'L': g_isRightKeyDown.store(true); return 1;
                case 'I': g_isUpKeyDown.store(true); return 1;
                case 'K': g_isDownKeyDown.store(true); return 1;

                    // 画面の内側4点へのジャンプ処理
                case 'W': MoveCursorToCorner(0); return 1; // 左上内側
                case 'X': MoveCursorToCorner(1); return 1; // 左下内側
                case 'O': MoveCursorToCorner(2); return 1; // 右上内側
                case VK_OEM_PERIOD: MoveCursorToCorner(3); return 1; // 右下内側 ('>' / '.' キー)

                case 'F':
                    if (!g_isMouseLeftDown.exchange(true)) {
                        SendMouseClick(MOUSEEVENTF_LEFTDOWN);
                    }
                    return 1;
                case 'G':
                    if (!g_isMouseMiddleDown.exchange(true)) {
                        SendMouseClick(MOUSEEVENTF_MIDDLEDOWN);
                    }
                    return 1;
                case 'R':
                    if (!g_isMouseRightDown.exchange(true)) {
                        SendMouseClick(MOUSEEVENTF_RIGHTDOWN);
                    }
                    return 1;

                case 'Y': {
                    int multiplier = g_isShiftKeyDown.load() ? 4 : 1;
                    SendMouseWheel(WHEEL_DELTA * multiplier);
                    return 1;
                }
                case 'H': {
                    int multiplier = g_isShiftKeyDown.load() ? 4 : 1;
                    SendMouseWheel(-WHEEL_DELTA * multiplier);
                    return 1;
                }
                }
            }
            else if (isKeyUp) {
                switch (pkbd->vkCode) {
                case 'J': g_isLeftKeyDown.store(false); return 1;
                case 'L': g_isRightKeyDown.store(false); return 1;
                case 'I': g_isUpKeyDown.store(false); return 1;
                case 'K': g_isDownKeyDown.store(false); return 1;

                case 'W':
                case 'X':
                case 'O':
                case VK_OEM_PERIOD:
                    return 1;

                case 'F':
                    if (g_isMouseLeftDown.exchange(false)) {
                        SendMouseClick(MOUSEEVENTF_LEFTUP);
                    }
                    return 1;
                case 'G':
                    if (g_isMouseMiddleDown.exchange(false)) {
                        SendMouseClick(MOUSEEVENTF_MIDDLEUP);
                    }
                    return 1;
                case 'R':
                    if (g_isMouseRightDown.exchange(false)) {
                        SendMouseClick(MOUSEEVENTF_RIGHTUP);
                    }
                    return 1;
                }
            }
        }
    }
    return CallNextHookEx(hLowLevelKeyboardHook, nCode, wParam, lParam);
}

void MouseProc(HWND hWnd) {
    UpdateTrayIcon(hWnd);

    // デルタタイム計算
    auto currentTime = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsedTime = currentTime - g_lastTime;
    double deltaTime = elapsedTime.count();
    g_lastTime = currentTime;

    if (!g_isKeyboardMouseEnabled.load()) return;

    // 移動方向判定
    int dirX = 0;
    int dirY = 0;
    if (g_isLeftKeyDown.load())  dirX -= 1;
    if (g_isRightKeyDown.load()) dirX += 1;
    if (g_isUpKeyDown.load())    dirY -= 1;
    if (g_isDownKeyDown.load())  dirY += 1;

    if (dirX == 0 && dirY == 0) return;

    // DPI取得
    static UINT dpi = 0;
    if (dpi == 0) {
        HDC hdc = GetDC(NULL);
        dpi = hdc ? GetDeviceCaps(hdc, LOGPIXELSX) : 96;
        if (hdc) ReleaseDC(NULL, hdc);
    }

    // 移動量計算
    double pixelsPerSecond = SPEED_INCHES_PER_SEC * static_cast<double>(dpi);
    if (g_isShiftKeyDown.load()) {
        pixelsPerSecond *= SHIFT_MULTI;
    }

    double moveX = dirX * pixelsPerSecond * deltaTime;
    double moveY = dirY * pixelsPerSecond * deltaTime;

    // 斜め移動補正
    if (dirX != 0 && dirY != 0) {
        moveX *= DIAGONAL_SCALE;
        moveY *= DIAGONAL_SCALE;
    }

    // 端数累積処理
    static double remainderX = 0.0;
    static double remainderY = 0.0;

    remainderX += moveX;
    remainderY += moveY;

    int actualMoveX = static_cast<int>(remainderX);
    int actualMoveY = static_cast<int>(remainderY);

    if (actualMoveX != 0 || actualMoveY != 0) {
        POINT pt;
        if (GetCursorPos(&pt)) {
            SetCursorPos(pt.x + actualMoveX, pt.y + actualMoveY);
            remainderX -= actualMoveX;
            remainderY -= actualMoveY;
        }
    }
}

void SendMouseClick(DWORD flags) {
    INPUT input = { 0 };
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = flags;
    SendInput(1, &input, sizeof(INPUT));
}

void SendMouseWheel(int scrollAmount) {
    INPUT input = { 0 };
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_WHEEL;
    input.mi.mouseData = static_cast<DWORD>(scrollAmount);
    SendInput(1, &input, sizeof(INPUT));
}
