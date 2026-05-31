#include <windows.h>
#include <tchar.h>
#include <stdio.h>
#include "../protocolo.h"

#define ID_CONFIGURACAO 1001
#define ID_ACERCA 1002
#define ID_SAIR 1003
#define IDC_MAX_ALERTAS 2001
#define IDC_SHM_NAME 2002
#define IDC_MUTEX_NAME 2003
#define IDC_EVENT_NAME 2004
#define IDC_BTN_OK 2005
#define IDC_BTN_PREV 2006
#define IDC_BTN_NEXT 2007

HANDLE hMapFile = NULL;
SHM_ALERTA* pDados = NULL;
HANDLE hEvent = NULL;
HANDLE hMutex = NULL;
HANDLE hThread = NULL;

int maxAlertas = 5;
int paginaAtual = 0;
HWND hMainWindow;
HWND hConfigWindow = NULL;
HWND hBtnPrev = NULL;
HWND hBtnNext = NULL;

TCHAR nomeSHM[256] = _T("Local\\SO2_SHM");
TCHAR nomeMutex[256] = _T("Local\\SO2_MUTEX");
TCHAR nomeEvento[256] = _T("Local\\SO2_EVENT");

HFONT hFontTitulo;
HFONT hFontNormal;
HFONT hFontBold;

void DesligarRecursos() {
    if (pDados) { UnmapViewOfFile(pDados); pDados = NULL; }
    if (hMapFile) { CloseHandle(hMapFile); hMapFile = NULL; }
    if (hEvent) { CloseHandle(hEvent); hEvent = NULL; }
    if (hMutex) { CloseHandle(hMutex); hMutex = NULL; }
}

BOOL LigarRecursos() {
    DesligarRecursos();
    hMapFile = OpenFileMapping(FILE_MAP_READ, FALSE, nomeSHM);
    if (hMapFile) pDados = (SHM_ALERTA*)MapViewOfFile(hMapFile, FILE_MAP_READ, 0, 0, sizeof(SHM_ALERTA));
    hEvent = OpenEvent(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, nomeEvento);
    hMutex = OpenMutex(SYNCHRONIZE, FALSE, nomeMutex);
    return (pDados && hEvent && hMutex);
}

DWORD WINAPI ThreadMonitorUpdates(LPVOID lpParam) {
    HWND hWnd = (HWND)lpParam;
    while (1) {
        if (hEvent == NULL) {
            Sleep(1000);
            continue;
        }
        DWORD res = WaitForSingleObject(hEvent, 1000);
        if (res == WAIT_OBJECT_0) {
            if (pDados && pDados->desligar) {
                PostMessage(hWnd, WM_CLOSE, 0, 0);
                break;
            }
            InvalidateRect(hWnd, NULL, FALSE);
            ResetEvent(hEvent);
        }
    }
    return 0;
}

LRESULT CALLBACK ConfigWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        CreateWindow(_T("STATIC"), _T("Max Alertas/Pagina:"), WS_CHILD | WS_VISIBLE, 10, 10, 150, 20, hWnd, NULL, NULL, NULL);
        CreateWindow(_T("EDIT"), _T("5"), WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER, 160, 10, 100, 20, hWnd, (HMENU)IDC_MAX_ALERTAS, NULL, NULL);
        CreateWindow(_T("STATIC"), _T("Nome SHM:"), WS_CHILD | WS_VISIBLE, 10, 40, 150, 20, hWnd, NULL, NULL, NULL);
        CreateWindow(_T("EDIT"), nomeSHM, WS_CHILD | WS_VISIBLE | WS_BORDER, 160, 40, 100, 20, hWnd, (HMENU)IDC_SHM_NAME, NULL, NULL);
        CreateWindow(_T("STATIC"), _T("Nome Mutex:"), WS_CHILD | WS_VISIBLE, 10, 70, 150, 20, hWnd, NULL, NULL, NULL);
        CreateWindow(_T("EDIT"), nomeMutex, WS_CHILD | WS_VISIBLE | WS_BORDER, 160, 70, 100, 20, hWnd, (HMENU)IDC_MUTEX_NAME, NULL, NULL);
        CreateWindow(_T("STATIC"), _T("Nome Evento:"), WS_CHILD | WS_VISIBLE, 10, 100, 150, 20, hWnd, NULL, NULL, NULL);
        CreateWindow(_T("EDIT"), nomeEvento, WS_CHILD | WS_VISIBLE | WS_BORDER, 160, 100, 100, 20, hWnd, (HMENU)IDC_EVENT_NAME, NULL, NULL);
        CreateWindow(_T("BUTTON"), _T("Guardar e Aplicar"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 60, 140, 150, 30, hWnd, (HMENU)IDC_BTN_OK, NULL, NULL);
        break;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_BTN_OK) {
            BOOL trans;
            int maxA = GetDlgItemInt(hWnd, IDC_MAX_ALERTAS, &trans, FALSE);
            if (trans && maxA > 0) maxAlertas = maxA;
            GetWindowText(GetDlgItem(hWnd, IDC_SHM_NAME), nomeSHM, 256);
            GetWindowText(GetDlgItem(hWnd, IDC_MUTEX_NAME), nomeMutex, 256);
            GetWindowText(GetDlgItem(hWnd, IDC_EVENT_NAME), nomeEvento, 256);
            LigarRecursos();
            paginaAtual = 0;
            InvalidateRect(hMainWindow, NULL, FALSE);
            DestroyWindow(hWnd);
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hWnd);
        break;
    case WM_DESTROY:
        hConfigWindow = NULL;
        break;
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

void AbrirConfiguracao(HINSTANCE hInstance) {
    if (hConfigWindow != NULL) {
        BringWindowToTop(hConfigWindow);
        return;
    }
    WNDCLASSEX wcex = { 0 };
    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.lpfnWndProc = ConfigWndProc;
    wcex.hInstance = hInstance;
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszClassName = _T("ConfigClass");
    RegisterClassEx(&wcex);
    hConfigWindow = CreateWindow(_T("ConfigClass"), _T("Configuracao do Monitor"), WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 300, 230, hMainWindow, NULL, hInstance, NULL);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    PAINTSTRUCT ps;
    HDC hdc;
    TCHAR buffer[512];
    int y = 80;

    switch (message) {
    case WM_CREATE:
    {
        HMENU hMenu = CreateMenu();
        HMENU hSubMenu = CreatePopupMenu();
        AppendMenu(hSubMenu, MF_STRING, ID_CONFIGURACAO, _T("Configuracao"));
        AppendMenu(hSubMenu, MF_STRING, ID_ACERCA, _T("Acerca"));
        AppendMenu(hSubMenu, MF_SEPARATOR, 0, NULL);
        AppendMenu(hSubMenu, MF_STRING, ID_SAIR, _T("Sair"));
        AppendMenu(hMenu, MF_POPUP, (UINT_PTR)hSubMenu, _T("Ficheiro"));
        SetMenu(hWnd, hMenu);

        hFontTitulo = CreateFont(28, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, _T("Segoe UI"));
        hFontNormal = CreateFont(20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, _T("Segoe UI"));
        hFontBold = CreateFont(18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, _T("Segoe UI"));

        hBtnPrev = CreateWindow(_T("BUTTON"), _T("< Anterior (PgUp)"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 480, 15, 140, 30, hWnd, (HMENU)IDC_BTN_PREV, NULL, NULL);
        hBtnNext = CreateWindow(_T("BUTTON"), _T("Seguinte (PgDn) >"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 630, 15, 140, 30, hWnd, (HMENU)IDC_BTN_NEXT, NULL, NULL);

        SendMessage(hBtnPrev, WM_SETFONT, (WPARAM)hFontBold, TRUE);
        SendMessage(hBtnNext, WM_SETFONT, (WPARAM)hFontBold, TRUE);
    }
    break;
    case WM_COMMAND:
        if (LOWORD(wParam) == ID_SAIR) {
            PostMessage(hWnd, WM_CLOSE, 0, 0);
        }
        else if (LOWORD(wParam) == ID_ACERCA) {
            MessageBox(hWnd, _T("Autores:\nDiogo Ribeiro Costa - 2024143983\nRodrigo Cravo Pereira - 2024117439\nSO2 ISEC"), _T("Acerca"), MB_OK | MB_ICONINFORMATION);
        }
        else if (LOWORD(wParam) == ID_CONFIGURACAO) {
            AbrirConfiguracao((HINSTANCE)GetWindowLongPtr(hWnd, GWLP_HINSTANCE));
        }
        else if (LOWORD(wParam) == IDC_BTN_PREV) {
            SendMessage(hWnd, WM_KEYDOWN, VK_PRIOR, 0);
            SetFocus(hWnd);
        }
        else if (LOWORD(wParam) == IDC_BTN_NEXT) {
            SendMessage(hWnd, WM_KEYDOWN, VK_NEXT, 0);
            SetFocus(hWnd);
        }
        break;
    case WM_KEYDOWN:
        if (wParam == VK_NEXT) {
            if (pDados && hMutex) {
                WaitForSingleObject(hMutex, INFINITE);
                int total = pDados->num_alertas;
                ReleaseMutex(hMutex);
                int maxPaginas = (total > 0) ? ((total - 1) / maxAlertas) : 0;
                if (paginaAtual < maxPaginas) {
                    paginaAtual++;
                    InvalidateRect(hWnd, NULL, FALSE);
                }
            }
        }
        else if (wParam == VK_PRIOR) {
            if (paginaAtual > 0) {
                paginaAtual--;
                InvalidateRect(hWnd, NULL, FALSE);
            }
        }
        break;
    case WM_PAINT:
    {
        hdc = BeginPaint(hWnd, &ps);
        RECT rcClient;
        GetClientRect(hWnd, &rcClient);

        HDC hdcMem = CreateCompatibleDC(hdc);
        HBITMAP hbmMem = CreateCompatibleBitmap(hdc, rcClient.right, rcClient.bottom);
        HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, hbmMem);

        HBRUSH hbrBg = CreateSolidBrush(RGB(245, 245, 250));
        FillRect(hdcMem, &rcClient, hbrBg);
        DeleteObject(hbrBg);

        RECT rcHeader = rcClient;
        rcHeader.bottom = 60;
        HBRUSH hbrHeader = CreateSolidBrush(RGB(30, 60, 100));
        FillRect(hdcMem, &rcHeader, hbrHeader);
        DeleteObject(hbrHeader);

        SetBkMode(hdcMem, TRANSPARENT);
        SelectObject(hdcMem, hFontTitulo);
        SetTextColor(hdcMem, RGB(255, 255, 255));
        _stprintf_s(buffer, 512, _T("Plataforma de Alertas - Pagina %d"), paginaAtual + 1);
        TextOut(hdcMem, 20, 12, buffer, (int)_tcslen(buffer));

        if (pDados && hMutex) {
            WaitForSingleObject(hMutex, INFINITE);
            int totalAlertas = pDados->num_alertas;
            int startIdx = paginaAtual * maxAlertas;

            if (totalAlertas == 0) {
                SelectObject(hdcMem, hFontNormal);
                SetTextColor(hdcMem, RGB(100, 100, 100));
                TextOut(hdcMem, 20, y, _T("Nenhum alerta ativo de momento."), 31);
            }
            else {
                for (int i = startIdx; i < startIdx + maxAlertas && i < totalAlertas; i++) {
                    RECT rcCard = { 20, y, rcClient.right - 20, y + 70 };
                    HBRUSH hbrCard = CreateSolidBrush(RGB(255, 255, 255));
                    HPEN hPenBorder = CreatePen(PS_SOLID, 1, RGB(200, 200, 200));
                    HPEN hOldPen = (HPEN)SelectObject(hdcMem, hPenBorder);
                    HBRUSH hOldBrush = (HBRUSH)SelectObject(hdcMem, hbrCard);

                    RoundRect(hdcMem, rcCard.left, rcCard.top, rcCard.right, rcCard.bottom, 10, 10);

                    SelectObject(hdcMem, hOldBrush);
                    SelectObject(hdcMem, hOldPen);
                    DeleteObject(hbrCard);
                    DeleteObject(hPenBorder);

                    SelectObject(hdcMem, hFontBold);
                    SetTextColor(hdcMem, RGB(80, 80, 80));
                    _stprintf_s(buffer, 512, _T("Placar %lu  |  Restam %lu seg"), pDados->alertas[i].identificador, pDados->alertas[i].duracao);
                    TextOut(hdcMem, 35, y + 10, buffer, (int)_tcslen(buffer));

                    SelectObject(hdcMem, hFontNormal);
                    SetTextColor(hdcMem, RGB(20, 20, 20));
                    _stprintf_s(buffer, 512, _T("%s"), pDados->alertas[i].msg);
                    TextOut(hdcMem, 35, y + 35, buffer, (int)_tcslen(buffer));

                    y += 85;
                }
            }
            ReleaseMutex(hMutex);
        }
        else {
            SelectObject(hdcMem, hFontNormal);
            SetTextColor(hdcMem, RGB(220, 50, 50));
            TextOut(hdcMem, 20, y, _T("A aguardar ligacao ao Central..."), 32);
        }

        BitBlt(hdc, 0, 0, rcClient.right, rcClient.bottom, hdcMem, 0, 0, SRCCOPY);
        SelectObject(hdcMem, hbmOld);
        DeleteObject(hbmMem);
        DeleteDC(hdcMem);

        EndPaint(hWnd, &ps);
        break;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_DESTROY:
        DesligarRecursos();
        DeleteObject(hFontTitulo);
        DeleteObject(hFontNormal);
        DeleteObject(hFontBold);
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    WNDCLASSEX wcex = { 0 };
    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = hInstance;
    wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcex.hbrBackground = NULL;
    wcex.lpszClassName = _T("MonitorClass");
    RegisterClassEx(&wcex);

    hMainWindow = CreateWindow(_T("MonitorClass"), _T("Monitor de Alertas"), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 800, 600, NULL, NULL, hInstance, NULL);

    LigarRecursos();
    hThread = CreateThread(NULL, 0, ThreadMonitorUpdates, hMainWindow, 0, NULL);

    ShowWindow(hMainWindow, nCmdShow);
    UpdateWindow(hMainWindow);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return (int)msg.wParam;
}