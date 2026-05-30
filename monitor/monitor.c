// monitor.c - Programa Monitor (Win32 GUI)
// Interface grafica para visualizar alertas ativos
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <tchar.h>
#include <stdio.h>
#include <stdlib.h>
#include <commctrl.h>
#include "../protocolo.h"
#include "resource.h"

// Nomes dos objetos partilhados (mesmos do central)
#define SHM_NAME        _T("Global\\TrabSO2_ShmAlerta")
#define EVT_UPDATE_NAME _T("Global\\TrabSO2_EvtUpdate")
#define MUTEX_SHM_NAME  _T("Global\\TrabSO2_MutexShm")

// Control IDs
#define ID_LISTVIEW     101
#define ID_MAX_ALERTAS  102
#define ID_STATUSBAR    103
#define ID_BTN_PREV     104
#define ID_BTN_NEXT     105
#define ID_LBL_PAGE     106

// Cores para UI
#define COLOR_URGENT    RGB(255, 0, 0)
#define COLOR_WARNING   RGB(255, 165, 0)
#define COLOR_NORMAL    RGB(0, 100, 200)

// Dados do grupo (autores)
#define AUTOR1_NOME     _T("Rodrigo Cravo Pereira")
#define AUTOR1_NUMERO   _T("2024117439")
#define AUTOR2_NOME     _T("Diogo Ribeiro Costa")
#define AUTOR2_NUMERO   _T("2024143983")

typedef struct {
    // Memoria partilhada
    HANDLE hMapFile;
    SHM_ALERTA* shm;
    HANDLE hEvtUpdate;
    HANDLE hMutexShm;
    HANDLE hThreadMonitor;

    // UI
    HWND hWndListView;
    HWND hStatusBar;
    HWND hBtnPrev;
    HWND hBtnNext;
    HWND hLblPage;
    HWND hDlgConfig;
    int maxAlertas;
    int paginaAtual;
    BOOL deveSair;
    HBRUSH hBrushBg;

    // Nomes configuráveis (para DialogBox)
    TCHAR shmName[256];
    TCHAR evtName[256];
    TCHAR mutexName[256];
} CONTEXTO_MONITOR;

static CONTEXTO_MONITOR g_ctx;

// Funcao para obter o numero total de alertas ativos
static int ContarAlertasAtivos(void) {
    int i, count = 0;
    if (g_ctx.shm == NULL) return 0;

    WaitForSingleObject(g_ctx.hMutexShm, INFINITE);
    for (i = 0; i < MAX_PLACAR; i++) {
        if (g_ctx.shm->placar[i].identificador != 0 &&
            g_ctx.shm->placar[i].msg[0] != _T('\0')) {
            count++;
        }
    }
    ReleaseMutex(g_ctx.hMutexShm);
    return count;
}

// Atualiza a ListView com os dados da SHM
static void AtualizarListView(HWND hWndLV) {
    int i, count, totalAlertas;
    int inicio, fim;
    LVITEM lvi;
    TCHAR buf[128];

    if (hWndLV == NULL) return;

    ListView_DeleteAllItems(hWndLV);

    if (g_ctx.shm == NULL) {
        SetDlgItemText(g_ctx.hStatusBar, 0, _T("Estado: Desconectado do servidor"));
        return;
    }

    WaitForSingleObject(g_ctx.hMutexShm, INFINITE);

    // Verificar se a plataforma foi encerrada
    if (g_ctx.shm->desligar) {
        ReleaseMutex(g_ctx.hMutexShm);
        SetDlgItemText(g_ctx.hStatusBar, 0, _T("Estado: Plataforma encerrada"));
        return;
    }

    // Contar alertas ativos
    count = 0;
    for (i = 0; i < MAX_PLACAR; i++) {
        if (g_ctx.shm->placar[i].identificador != 0 &&
            g_ctx.shm->placar[i].msg[0] != _T('\0')) {
            count++;
        }
    }
    totalAlertas = count;

    // Calcular paginacao
    if (g_ctx.maxAlertas <= 0) g_ctx.maxAlertas = 10;
    inicio = g_ctx.paginaAtual * g_ctx.maxAlertas;
    fim = inicio + g_ctx.maxAlertas;
    if (fim > count) fim = count;

    // Atualizar status bar
    int totalPaginas = (totalAlertas + g_ctx.maxAlertas - 1) / g_ctx.maxAlertas;
    if (totalPaginas == 0) totalPaginas = 1;
    _stprintf_s(buf, _countof(buf), _T("Alertas: %d  |  Página: %d/%d"), 
                totalAlertas, g_ctx.paginaAtual + 1, totalPaginas);
    SetDlgItemText(g_ctx.hStatusBar, 0, buf);

    // Habilitar/desabilitar botões de navegação
    EnableWindow(g_ctx.hBtnPrev, g_ctx.paginaAtual > 0);
    EnableWindow(g_ctx.hBtnNext, g_ctx.paginaAtual < totalPaginas - 1);

    // Preencher ListView apenas com os itens da pagina atual
    count = 0;
    for (i = 0; i < MAX_PLACAR; i++) {
        if (g_ctx.shm->placar[i].identificador != 0 &&
            g_ctx.shm->placar[i].msg[0] != _T('\0')) {

            if (count >= inicio && count < fim) {
                ZeroMemory(&lvi, sizeof(lvi));
                lvi.mask = LVIF_TEXT;
                lvi.iItem = count - inicio;

                // Coluna 0: Identificador
                _stprintf_s(buf, _countof(buf), _T("Placar #%lu"), g_ctx.shm->placar[i].identificador);
                lvi.iSubItem = 0;
                lvi.pszText = buf;
                ListView_InsertItem(hWndLV, &lvi);

                // Coluna 1: Mensagem
                lvi.iSubItem = 1;
                lvi.pszText = g_ctx.shm->placar[i].msg;
                ListView_SetItem(hWndLV, &lvi);

                // Coluna 2: Duracao
                _stprintf_s(buf, _countof(buf), _T("%lu seg"), g_ctx.shm->placar[i].duracao);
                lvi.iSubItem = 2;
                lvi.pszText = buf;
                ListView_SetItem(hWndLV, &lvi);
            }
            count++;
        }
    }

    ReleaseMutex(g_ctx.hMutexShm);
}

// Thread que monitoriza alteracoes na SHM
static DWORD WINAPI ThreadMonitorizacao(LPVOID param) {
    HWND hWndLV = (HWND)param;
    HANDLE eventos[2];
    DWORD res;

    eventos[0] = g_ctx.hEvtUpdate;
    eventos[1] = OpenEvent(EVENT_ALL_ACCESS, FALSE, EVT_UPDATE_NAME);
    if (eventos[1] == NULL) eventos[1] = g_ctx.hEvtUpdate;

    while (!g_ctx.deveSair) {
        res = WaitForMultipleObjects(2, eventos, FALSE, 500);
        if (res == WAIT_OBJECT_0 || res == WAIT_OBJECT_0 + 1) {
            // Houve alteracao - atualizar ListView
            AtualizarListView(hWndLV);
        }

        // Verificar desligamento
        if (g_ctx.shm != NULL) {
            WaitForSingleObject(g_ctx.hMutexShm, INFINITE);
            BOOL desligar = g_ctx.shm->desligar;
            ReleaseMutex(g_ctx.hMutexShm);
            if (desligar) {
                // Se a plataforma encerrou, podemos continuar (sem dados)
                // A interface mostra "vazio" mas nao termina
            }
        }
    }

    if (eventos[1] != g_ctx.hEvtUpdate) CloseHandle(eventos[1]);
    return 0;
}

// DialogBox de configuracao
static INT_PTR CALLBACK DlgConfigProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    TCHAR buf[64];

    switch (msg) {
    case WM_INITDIALOG:
        // Preencher campos com valores atuais
        _stprintf_s(buf, _countof(buf), _T("%d"), g_ctx.maxAlertas);
        SetDlgItemText(hDlg, IDC_EDIT1, buf);
        SetDlgItemText(hDlg, IDC_EDIT2, g_ctx.shmName);
        SetDlgItemText(hDlg, IDC_EDIT3, g_ctx.evtName);
        SetDlgItemText(hDlg, IDC_EDIT4, g_ctx.mutexName);
        return TRUE;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            // Ler o numero maximo de alertas
            GetDlgItemText(hDlg, IDC_EDIT1, buf, _countof(buf));
            g_ctx.maxAlertas = _tstoi(buf);
            if (g_ctx.maxAlertas < 1) g_ctx.maxAlertas = 1;
            if (g_ctx.maxAlertas > MAX_PLACAR) g_ctx.maxAlertas = MAX_PLACAR;

            // Ler nomes dos recursos (para reabertura, mas simplificamos)
            GetDlgItemText(hDlg, IDC_EDIT2, g_ctx.shmName, _countof(g_ctx.shmName));
            GetDlgItemText(hDlg, IDC_EDIT3, g_ctx.evtName, _countof(g_ctx.evtName));
            GetDlgItemText(hDlg, IDC_EDIT4, g_ctx.mutexName, _countof(g_ctx.mutexName));

            g_ctx.paginaAtual = 0;
            AtualizarListView(g_ctx.hWndListView);
            EndDialog(hDlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

// Janela principal
static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    int totalAlertas, totalPaginas;

    switch (msg) {
    case WM_CREATE: {
        // Criar ListView
        HINSTANCE hInst = ((LPCREATESTRUCT)lParam)->hInstance;
        INITCOMMONCONTROLSEX icex;
        icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
        icex.dwICC = ICC_LISTVIEW_CLASSES | ICC_HOTKEY_CLASS | ICC_BAR_CLASSES;
        InitCommonControlsEx(&icex);

        // Criar status bar
        g_ctx.hStatusBar = CreateWindowEx(0, STATUSCLASSNAME, NULL,
            WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
            0, 0, 0, 0, hWnd, (HMENU)ID_STATUSBAR, hInst, NULL);
        
        SendMessage(g_ctx.hStatusBar, SB_SETTEXT, 0, (LPARAM)_T("Conectando..."));

        // Criar ListView com melhor visual
        g_ctx.hWndListView = CreateWindow(WC_LISTVIEW, _T(""),
            WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL |
            LVS_REPORT | LVS_SINGLESEL | LVS_NOSORTHEADER | LVS_EX_GRIDLINES | LVS_EX_FULLROWSELECT,
            10, 45, 600, 350,
            hWnd, (HMENU)ID_LISTVIEW, hInst, NULL);

        if (g_ctx.hWndListView == NULL) return -1;

        // Aplicar estilos extendidos
        DWORD dwExStyle = LVS_EX_GRIDLINES | LVS_EX_FULLROWSELECT | LVS_EX_HEADERDRAGDROP;
        ListView_SetExtendedListViewStyle(g_ctx.hWndListView, dwExStyle);

        // Configurar colunas com melhor espaçamento
        LVCOLUMN lvc;
        TCHAR headers[][32] = {
            _T("Placar"),
            _T("Mensagem"),
            _T("Duracao")
        };
        int i;
        for (i = 0; i < 3; i++) {
            ZeroMemory(&lvc, sizeof(lvc));
            lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
            lvc.pszText = headers[i];
            lvc.cx = (i == 0) ? 100 : (i == 1) ? 400 : 80;
            ListView_InsertColumn(g_ctx.hWndListView, i, &lvc);
        }

        // Botões de navegação
        g_ctx.hBtnPrev = CreateWindow(_T("BUTTON"), _T("< Anterior"),
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            10, 410, 140, 28, hWnd, (HMENU)ID_BTN_PREV, hInst, NULL);

        g_ctx.hBtnNext = CreateWindow(_T("BUTTON"), _T("Proximo >"),
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            160, 410, 140, 28, hWnd, (HMENU)ID_BTN_NEXT, hInst, NULL);

        // Label de página
        g_ctx.hLblPage = CreateWindow(_T("STATIC"), _T("Página 1/1"),
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            310, 410, 300, 28, hWnd, (HMENU)ID_LBL_PAGE, hInst, NULL);

        // Criar menu melhorado
        HMENU hMenu = CreateMenu();
        HMENU hFileMenu = CreatePopupMenu();
        HMENU hViewMenu = CreatePopupMenu();
        
        AppendMenu(hFileMenu, MF_STRING, 1001, _T("Configuracao"));
        AppendMenu(hFileMenu, MF_STRING, 1002, _T("Acerca"));
        AppendMenu(hFileMenu, MF_SEPARATOR, 0, NULL);
        AppendMenu(hFileMenu, MF_STRING, 1003, _T("Sair"));
        
        AppendMenu(hViewMenu, MF_STRING, 2001, _T("Atualizar Agora (F5)"));
        AppendMenu(hViewMenu, MF_STRING, 2002, _T("Primeira Pagina (Home)"));
        AppendMenu(hViewMenu, MF_STRING, 2003, _T("Ultima Pagina (End)"));
        
        AppendMenu(hMenu, MF_POPUP, (UINT_PTR)hFileMenu, _T("Ficheiro"));
        AppendMenu(hMenu, MF_POPUP, (UINT_PTR)hViewMenu, _T("Ver"));
        
        SetMenu(hWnd, hMenu);

        // Iniciar thread de monitorizacao
        g_ctx.hThreadMonitor = CreateThread(NULL, 0, ThreadMonitorizacao, g_ctx.hWndListView, 0, NULL);

        // Atualizar pela primeira vez
        AtualizarListView(g_ctx.hWndListView);
        break;
    }

    case WM_SIZE: {
        RECT rc;
        GetClientRect(hWnd, &rc);
        
        // Redimensionar ListView
        if (g_ctx.hWndListView) {
            SetWindowPos(g_ctx.hWndListView, NULL,
                10, 45, rc.right - 20, rc.bottom - 130,
                SWP_NOZORDER);
        }
        
        // Redimensionar botões
        if (g_ctx.hBtnPrev) {
            SetWindowPos(g_ctx.hBtnPrev, NULL,
                10, rc.bottom - 75, 140, 28,
                SWP_NOZORDER);
        }
        if (g_ctx.hBtnNext) {
            SetWindowPos(g_ctx.hBtnNext, NULL,
                160, rc.bottom - 75, 140, 28,
                SWP_NOZORDER);
        }
        if (g_ctx.hLblPage) {
            SetWindowPos(g_ctx.hLblPage, NULL,
                310, rc.bottom - 75, rc.right - 320, 28,
                SWP_NOZORDER);
        }
        
        // Redimensionar status bar (automático)
        SendMessage(g_ctx.hStatusBar, WM_SIZE, 0, 0);
        break;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case 1001: // Configuracao
            DialogBox(GetModuleHandle(NULL),
                MAKEINTRESOURCE(IDD_DIALOG1),
                hWnd, DlgConfigProc);
            break;

        case 1002: { // Acerca
            TCHAR msg[512];
            _stprintf_s(msg, _countof(msg),
                _T("Trabalho SO2 - Monitor de Alertas\n\n")
                _T("Plataforma de Informação com Placares\n")
                _T("Módulo 3 - Memória Partilhada e Eventos\n\n")
                _T("Autores:\n")
                _T("  %s - %s\n")
                _T("  %s - %s\n\n")
                _T("© 2026 Universidade"),
                AUTOR1_NOME, AUTOR1_NUMERO,
                AUTOR2_NOME, AUTOR2_NUMERO);
            MessageBox(hWnd, msg, _T("Acerca do Monitor"), MB_OK | MB_ICONINFORMATION);
            break;
        }

        case 1003: // Sair
            DestroyWindow(hWnd);
            break;

        case 2001: // Atualizar agora
            if (g_ctx.hWndListView) {
                AtualizarListView(g_ctx.hWndListView);
            }
            break;

        case 2002: // Primeira página
            g_ctx.paginaAtual = 0;
            if (g_ctx.hWndListView) {
                AtualizarListView(g_ctx.hWndListView);
            }
            break;

        case 2003: { // Última página
            totalAlertas = ContarAlertasAtivos();
            totalPaginas = (totalAlertas + g_ctx.maxAlertas - 1) / g_ctx.maxAlertas;
            if (totalPaginas == 0) totalPaginas = 1;
            g_ctx.paginaAtual = totalPaginas - 1;
            if (g_ctx.hWndListView) {
                AtualizarListView(g_ctx.hWndListView);
            }
            break;
        }

        case ID_BTN_PREV: // Botão Anterior
            if (g_ctx.paginaAtual > 0) {
                g_ctx.paginaAtual--;
                AtualizarListView(g_ctx.hWndListView);
            }
            break;

        case ID_BTN_NEXT: { // Botão Próximo
            totalAlertas = ContarAlertasAtivos();
            totalPaginas = (totalAlertas + g_ctx.maxAlertas - 1) / g_ctx.maxAlertas;
            if (totalPaginas == 0) totalPaginas = 1;
            if (g_ctx.paginaAtual < totalPaginas - 1) {
                g_ctx.paginaAtual++;
                AtualizarListView(g_ctx.hWndListView);
            }
            break;
        }
        }
        break;

    case WM_KEYDOWN:
        if (wParam == VK_NEXT) { // Page Down
            totalAlertas = ContarAlertasAtivos();
            totalPaginas = (totalAlertas + g_ctx.maxAlertas - 1) / g_ctx.maxAlertas;
            if (totalPaginas == 0) totalPaginas = 1;
            if (g_ctx.paginaAtual < totalPaginas - 1) {
                g_ctx.paginaAtual++;
                AtualizarListView(g_ctx.hWndListView);
            }
        }
        else if (wParam == VK_PRIOR) { // Page Up
            if (g_ctx.paginaAtual > 0) {
                g_ctx.paginaAtual--;
                AtualizarListView(g_ctx.hWndListView);
            }
        }
        else if (wParam == VK_HOME) { // Home
            g_ctx.paginaAtual = 0;
            AtualizarListView(g_ctx.hWndListView);
        }
        else if (wParam == VK_END) { // End
            totalAlertas = ContarAlertasAtivos();
            totalPaginas = (totalAlertas + g_ctx.maxAlertas - 1) / g_ctx.maxAlertas;
            if (totalPaginas == 0) totalPaginas = 1;
            g_ctx.paginaAtual = totalPaginas - 1;
            AtualizarListView(g_ctx.hWndListView);
        }
        else if (wParam == VK_F5) { // F5
            AtualizarListView(g_ctx.hWndListView);
        }
        break;

    case WM_DESTROY:
        g_ctx.deveSair = TRUE;
        if (g_ctx.hThreadMonitor) {
            WaitForSingleObject(g_ctx.hThreadMonitor, 1000);
            CloseHandle(g_ctx.hThreadMonitor);
        }
        if (g_ctx.hBrushBg) DeleteObject(g_ctx.hBrushBg);
        PostQuitMessage(0);
        break;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

int WINAPI _tWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPTSTR lpCmdLine, int nCmdShow) {
    WNDCLASSEX wc;
    HWND hWnd;
    MSG msg;

    // Inicializar contexto
    ZeroMemory(&g_ctx, sizeof(g_ctx));
    g_ctx.maxAlertas = 10;
    g_ctx.paginaAtual = 0;
    g_ctx.deveSair = FALSE;

    // Nomes padrao
    _tcsncpy_s(g_ctx.shmName, _countof(g_ctx.shmName), SHM_NAME, _TRUNCATE);
    _tcsncpy_s(g_ctx.evtName, _countof(g_ctx.evtName), EVT_UPDATE_NAME, _TRUNCATE);
    _tcsncpy_s(g_ctx.mutexName, _countof(g_ctx.mutexName), MUTEX_SHM_NAME, _TRUNCATE);

    // Abrir memoria partilhada (criada pelo central)
    g_ctx.hMapFile = OpenFileMapping(FILE_MAP_ALL_ACCESS, FALSE, SHM_NAME);
    if (g_ctx.hMapFile != NULL) {
        g_ctx.shm = (SHM_ALERTA*)MapViewOfFile(g_ctx.hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SHM_ALERTA));
    }

    // Abrir evento de notificacao (criado pelo central)
    g_ctx.hEvtUpdate = OpenEvent(EVENT_ALL_ACCESS, FALSE, EVT_UPDATE_NAME);
    if (g_ctx.hEvtUpdate == NULL) {
        g_ctx.hEvtUpdate = CreateEvent(NULL, TRUE, FALSE, EVT_UPDATE_NAME);
    }

    // Abrir mutex (criado pelo central)
    g_ctx.hMutexShm = OpenMutex(MUTEX_ALL_ACCESS, FALSE, MUTEX_SHM_NAME);
    if (g_ctx.hMutexShm == NULL) {
        g_ctx.hMutexShm = CreateMutex(NULL, FALSE, MUTEX_SHM_NAME);
    }

    // Registrar classe da janela
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = _T("MonitorSO2");

    if (!RegisterClassEx(&wc)) return 1;

    // Criar janela
    hWnd = CreateWindow(_T("MonitorSO2"), _T("Monitor - Placar Informativo"),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        650, 500,
        NULL, NULL, hInstance, NULL);

    if (hWnd == NULL) return 1;

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    // Loop de mensagens
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Limpeza
    g_ctx.deveSair = TRUE;
    if (g_ctx.shm != NULL) UnmapViewOfFile(g_ctx.shm);
    if (g_ctx.hMapFile != NULL) CloseHandle(g_ctx.hMapFile);
    if (g_ctx.hEvtUpdate != NULL) CloseHandle(g_ctx.hEvtUpdate);
    if (g_ctx.hMutexShm != NULL) CloseHandle(g_ctx.hMutexShm);

    return (int)msg.wParam;
}