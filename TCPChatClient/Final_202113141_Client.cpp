/*
<개요>
- 윈도우 소켓 TCP 채팅 프로그램 클라이언트
- Win32 API GUI 기반
- 스레드 구성은 다음과 같음
  * 메인 네트워크 스레드(main) - connect()
  * 클라이언트 스레드(CliendMain)
	* 읽기(recv) 스레드(ReadThread)
	* 쓰기(send) 스레드(WriteThread)

<참고·참조한 자료들>
- 10주차 스레드 프로그래밍 활용
- 13주차 GUI 기반 소켓 프로그래밍
- 14주차 채팅프로그램 제작 -- 주로 이걸 참조함
*/

#define _CRT_SECURE_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#pragma comment(lib, "ws2_32")
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include "resource.h"
#include "..\Final_202113141_Packet.h"
#include "..\Final_202113141_Queue.h"

#define WM_CONNECT (WM_USER + 1000)
#define WM_DISCONNECT (WM_USER + 1001)

static HINSTANCE g_hInst; // 응용 프로그램 인스턴스 핸들
static HWND g_hDlg;
static HMENU g_hMenu;
static HWND g_hButtonConnect; // '접속' 버튼
static HWND g_hButtonSendMsg; // '보내기' 버튼
static HWND g_hEditAddress; // 
static HWND g_hEditPort; // 
static HWND g_hEditLog; // 받은 메시지 출력
static HWND g_hEditInput; // 
static HWND g_hListUser; // 
static char g_ipaddr[64]; // 서버 IP 주소
static u_short g_port; // 서버 포트 번호
static HANDLE g_hClientThread; // 스레드 핸들
static SOCKET g_sock; // 클라이언트 소켓
static HANDLE g_hReadEvent, g_hWriteEvent; // 이벤트 핸들
static HANDLE g_hReadThread, g_hWriteThread; // 스레드 핸들
static LPCSTR g_lpChangeNameDlg;
static Queue<Packet<C2SPacketType>*> g_sendQueue;
static CRITICAL_SECTION wcs;

static volatile int g_id = -1;
static char g_userName[MAXUSER][MAX_PACKET_SIZE];

// 대화상자 프로시저
BOOL CALLBACK DlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam);
// 이름 변경 창 대화상자 프로시저
BOOL CALLBACK DlgProcChangeName(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam);
// 소켓 통신 스레드 함수
DWORD WINAPI ClientMain(LPVOID arg);
DWORD WINAPI ReadThread(LPVOID arg);
DWORD WINAPI WriteThread(LPVOID arg);
// 편집 컨트롤 출력 함수
void DisplayText(const char* fmt, ...);
// 사용자 정의 데이터 수신 함수
int recvn(SOCKET s, char* buf, int len, int flags);
// 오류 출력 함수
void err_quit(const char* msg);
void err_display(const char* msg);

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // 윈속 초기화
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;
    // 이벤트 생성
    g_hReadEvent = CreateEvent(NULL, FALSE, TRUE, NULL);
    if (g_hReadEvent == NULL) return 1;
    g_hWriteEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (g_hWriteEvent == NULL) return 1;
    g_hInst = hInstance;
    // DisplayText용 임계 영역 초기화
    InitializeCriticalSection(&wcs);
    // 대화상자 생성
    DialogBox(hInstance, MAKEINTRESOURCE(IDD_DIALOG_CHAT), NULL, (DLGPROC)&DlgProc);
    // DisplayText용 임계 영역 해제
    DeleteCriticalSection(&wcs);
    // 이벤트 제거
    CloseHandle(g_hReadEvent);
    CloseHandle(g_hWriteEvent);
    // 윈속 종료
    WSACleanup();
    return 0;

}
// 대화상자 프로시저
BOOL CALLBACK DlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    int retval;
    switch (uMsg) {
    case WM_INITDIALOG:
        // 컨트롤 초기화
        g_hDlg = hDlg;
        g_hMenu = GetMenu(hDlg);
        g_hEditAddress = GetDlgItem(hDlg, IDC_EDIT_ADDRESS);
        g_hEditPort = GetDlgItem(hDlg, IDC_EDIT_PORT);
        g_hEditLog = GetDlgItem(hDlg, IDC_EDIT_LOG);
        g_hEditInput = GetDlgItem(hDlg, IDC_EDIT_INPUT);
        g_hButtonConnect = GetDlgItem(hDlg, IDC_BUTTON_CONNECT);
        g_hButtonSendMsg = GetDlgItem(hDlg, IDC_BUTTON_SENDMSG);
        g_hListUser = GetDlgItem(hDlg, IDC_LIST_USERLIST);
        SetDlgItemText(hDlg, IDC_EDIT_ADDRESS, "127.0.0.1");
        SetDlgItemText(hDlg, IDC_EDIT_PORT, "9000");
        // 변수 초기화
        g_lpChangeNameDlg = MAKEINTRESOURCE(IDD_DIALOG_CHANGE_NAME);
        for (int i = 0; i < MAXUSER; i++) {
            g_userName[i][0] = 0;
        }
        g_id = -1;
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_BUTTON_CONNECT:
            if (g_id >= 0) { // 연결 끊기 버튼 눌렀을 때 (연결 중인 경우)
                // 읽기 스레드 terminate (자동으로 쓰기 스레드도 끊김)
                TerminateThread(g_hReadThread, 1);
                return TRUE;
            }
            // 접속 버튼 눌렀을 때
            // IP 가져오기
            GetDlgItemText(hDlg, IDC_EDIT_ADDRESS, g_ipaddr, sizeof(g_ipaddr)); 
            // 포트 가져오기
            g_port = GetDlgItemInt(hDlg, IDC_EDIT_PORT, NULL, FALSE); 
            // 메시지 출력
            DisplayText("%s:%d에 연결 시도 중...\n", g_ipaddr, g_port);
            // 소켓 통신 스레드 시작
            g_hClientThread = CreateThread(NULL, 0, ClientMain, NULL, 0, NULL);
            if (g_hClientThread == NULL) {
                MessageBox(hDlg, 
                    "클라이언트를 시작할 수 없습니다." "\r\n프로그램을 종료합니다.", 
                    "실패!", 
                    MB_ICONERROR
                );
                EndDialog(hDlg, 0);
                return TRUE;
            }
            EnableWindow(g_hButtonConnect, FALSE);
            return TRUE;
        case IDC_BUTTON_SENDMSG: { // 보내기 버튼
            // 읽기 완료를 기다림
            WaitForSingleObject(g_hReadEvent, INFINITE);
            char message[MAX_STRING_SIZE];
            GetDlgItemText(hDlg, IDC_EDIT_INPUT, message, MAX_STRING_SIZE);
            message[MAX_STRING_SIZE - 1] = 0; // 혹시 모르니...
            int length = strlen(message);
            C2SChat* chatPacket = new C2SChat(message);
            g_sendQueue.enqueue(chatPacket);
            // 쓰기 완료를 알림
            SetEvent(g_hWriteEvent);
            // 입력된 텍스트 전체를 선택
            SetFocus(g_hEditInput);
            SendMessage(g_hEditInput, EM_SETSEL, 0, -1);
            return TRUE;
        }
        case ID_MENU_CHANGE_NAME: // 이름 변경 메뉴 선택
            retval = DialogBox(g_hInst, g_lpChangeNameDlg, NULL, (DLGPROC)&DlgProcChangeName);
            break;
        case IDCANCEL: // 이거 정의해줘야 ESC/닫기버튼 작동
            EndDialog(hDlg, 0);
            return TRUE;
        }
        return FALSE;
    case WM_CONNECT: // 연결 시
        EnableWindow(g_hButtonConnect, TRUE);
        g_id = wParam;
        if (g_id < 0) { // 연결 실패 시
            switch (g_id) {
            case FAILED_TO_CONNECT:
                DisplayText("[오류] 연결에 실패했습니다.\n");
                return TRUE;
            case SERVER_IS_FULL:
                DisplayText("[오류] 서버가 꽉 찼습니다.\n");
                return TRUE;
            case THREAD_ERROR:
                DisplayText("[오류] 스레드 생성에 실패했습니다.\n");
                return TRUE;
            }
            return TRUE;
        }
        EnableWindow(g_hEditAddress, FALSE);    // 주소 비활성화
        EnableWindow(g_hEditPort, FALSE);       // 포트 비활성화
        EnableWindow(g_hButtonSendMsg, TRUE);   // 보내기버튼 활성화
        EnableWindow(g_hEditInput, TRUE);       // 입력창 활성화
        EnableWindow(g_hListUser, TRUE);        // 리스트박스 활성화
        EnableMenuItem(g_hMenu, ID_MENU_CHANGE_NAME, MF_ENABLED); // 이름 바꾸기 메뉴 활성화
        SetDlgItemText(hDlg, IDC_BUTTON_CONNECT, "연결 끊기");    // 접속 버튼 연결 끊기로 캡션 변경
        sprintf(g_userName[g_id], "익명 %d", g_id + 1);           // 이름 변경
        SetWindowText(hDlg, g_userName[g_id]);                    // 대화상자 제목 이름으로 변경
        // 우측 리스트박스에 자신 넣기
        SendMessage(g_hListUser, LB_INITSTORAGE, MAXUSER, MAX_PACKET_SIZE);
        SendMessage(g_hListUser, LB_INSERTSTRING, 0, (LPARAM)g_userName[g_id]);
        // 입력창에 포커스
        SetFocus(g_hEditInput);
        // 보내기 버튼을 기본 버튼으로
        SendMessage(hDlg, DM_SETDEFID, IDC_BUTTON_SENDMSG, 0); 
        return TRUE;
    case WM_DISCONNECT:
        EnableWindow(g_hEditAddress, TRUE);     // 주소 활성화
        EnableWindow(g_hEditPort, TRUE);        // 포트 활성화
        EnableWindow(g_hButtonSendMsg, FALSE);  // 보내기 버튼 비활성화
        EnableWindow(g_hEditInput, FALSE);      // 입력창 비활성화
        EnableWindow(g_hListUser, FALSE);       // 리스트박스 비활성화
        EnableMenuItem(g_hMenu, ID_MENU_CHANGE_NAME, MF_DISABLED);  // 이름 바꾸기 메뉴 비활성화
        SetDlgItemText(hDlg, IDC_BUTTON_CONNECT, "접속");           // 연결 끊기 버튼 접속으로 캡션 변경
        SendMessage(g_hListUser, LB_RESETCONTENT, 0, 0);            // 리스트박스 초기화
        SetWindowText(g_hDlg, "채팅 클라이언트");                   // 대화상자 제목 초기화
        break;
    }
    return FALSE;
}


BOOL CALLBACK DlgProcChangeName(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    static HWND hEditChangeName;
    switch (uMsg) {
    case WM_INITDIALOG:
        hEditChangeName = GetDlgItem(hDlg, IDC_EDIT_CHANGE_NAME);
        // 이름 변경칸을 현재 닉네임으로 채움
        SetDlgItemText(hDlg, IDC_EDIT_CHANGE_NAME, g_userName[g_id]);
        // 이름 변경칸에 포커스
        SetFocus(hEditChangeName);
        // 이름 변경칸 전체 선택
        SendMessage(hEditChangeName, EM_SETSEL, 0, -1);
        break;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDOK: {
            char buf[MAX_STRING_SIZE] = { 0, };
            GetDlgItemText(hDlg, IDC_EDIT_CHANGE_NAME, buf, MAX_STRING_SIZE);
            buf[MAX_STRING_SIZE - 1] = 0;
            int length = strlen(buf);
            // 닉네임 없거나 텅 비었으면
            if (length <= 0 || strcmp(buf, g_userName[g_id]) == 0) {
                EndDialog(hDlg, 0);
                return 0;
            }
            // 읽기 완료를 기다림
            WaitForSingleObject(g_hReadEvent, INFINITE);
            // 패킷에 씀
            C2SChangeName* namePacket = new C2SChangeName(buf);
            g_sendQueue.enqueue(namePacket);
            // 쓰기 완료를 알림
            SetEvent(g_hWriteEvent);
            EndDialog(hDlg, 0);
            break;
        }
        case IDCANCEL:
            EndDialog(hDlg, 0);
            break;
        }
        break;
    }
    return 0;
}

// 소켓 통신 스레드 함수
DWORD WINAPI ClientMain(LPVOID arg) {
    int retval;

    // socket()
    g_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (g_sock == INVALID_SOCKET) err_quit("socket()");

    // connect()
    SOCKADDR_IN serveraddr;
    ZeroMemory(&serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = inet_addr(g_ipaddr);
    serveraddr.sin_port = htons(g_port);
    retval = connect(g_sock, (SOCKADDR*)&serveraddr, sizeof(serveraddr));
    if (retval == SOCKET_ERROR) {
        SendMessage(g_hDlg, WM_CONNECT, FAILED_TO_CONNECT, 0);
        err_display("connect()");
        return 1;
    }

    // 읽기 & 쓰기 스레드 생성
    HANDLE hThread[2];
    g_hReadThread = hThread[0] = CreateThread(NULL, 0, ReadThread, NULL, 0, NULL);
    g_hWriteThread = hThread[1] = CreateThread(NULL, 0, WriteThread, NULL, 0, NULL);
    if (hThread[0] == NULL || hThread[1] == NULL) {
        SendMessage(g_hDlg, WM_CONNECT, THREAD_ERROR, 0);
        return 1;
    }

    // 스레드 종료 대기
    retval = WaitForMultipleObjects(2, hThread, FALSE, INFINITE);
    retval -= WAIT_OBJECT_0;
    if (retval == 0)
        TerminateThread(hThread[1], 1);
    else
        TerminateThread(hThread[0], 1);
    CloseHandle(hThread[0]);
    CloseHandle(hThread[1]);
    DisplayText("[알림] 서버와의 접속이 끊겼습니다.\r\n");
    closesocket(g_sock);
    SendMessage(g_hDlg, WM_DISCONNECT, 0, 0);
    // 쌓여있던 큐 비우기
    while (!g_sendQueue.isEmpty()) {
        delete g_sendQueue.dequeue();
    }
    CloseHandle(g_hClientThread);
    g_id = -1;
    return 0;
}

// 데이터 받기
DWORD WINAPI ReadThread(LPVOID arg) {
    int retval;
    int type, size;
    char* buffer;
    S2CPacketType typeEnum;
    PacketReceiveData prd;
    while (1) {
        retval = recvn(g_sock, (char*)&type, sizeof(type), 0);
        if (retval == SOCKET_ERROR) { // 오류 발생하면
            err_display("recvn()"); // 출력
            break;
        } else if (retval == 0) { // 연결 종료 시
            break;
        }
        if (type <= (int)S2CPacketType::_Start
            || type >= (int)S2CPacketType::_End
        ) {
            DisplayText("[error] invalid range of type: %d", type);
            break;
        }
        typeEnum = (S2CPacketType)type;

        retval = recvn(g_sock, (char*)&size, sizeof(size), 0);
        if (retval == SOCKET_ERROR) { // 오류 발생하면
            err_display("recvn()"); // 출력
            break;
        } else if (retval == 0) { // 연결 종료 시
            break;
        }
        if (size <= 0
            || size >= MAX_PACKET_SIZE
        ) {
            DisplayText("[error] invalid packet size: %d", size);
            break;
        }

        buffer = new char[size];
        retval = recvn(g_sock, buffer, size, 0);
        if (retval == SOCKET_ERROR) { // 오류 발생하면
            err_display("recvn()"); // 출력
            break;
        } else if (retval == 0) { // 연결 종료 시
            break;
        }
        prd = { type, size, buffer };
        
        switch (typeEnum) {
        case S2CPacketType::Handshake: {
            S2CHandshake handshakePacket(prd); // buffer로부터 역직렬화
            SendMessage(g_hDlg, WM_CONNECT, handshakePacket.id, 0);
            DisplayText("[알림] 채팅방에 참가했습니다.\r\n");
            if (handshakePacket.userCount > 0) {
                DisplayText("[알림] %d명이 채팅 중입니다.\r\n", handshakePacket.userCount);
            } else {
                DisplayText("[알림] 채팅방에 혼자 있습니다.\r\n");
            }
            break;
        }
        case S2CPacketType::AddUser: {
            S2CAddUser addUserPacket(prd);
            char* name = addUserPacket.name;
            strcpy(g_userName[addUserPacket.id], addUserPacket.name);
            if ((addUserPacket.flag & AU_INITIALIZING) == 0) { // 새 유저의 접속이면
                DisplayText(">> %s\r\n", name);
            }
            SendMessage(g_hListUser, LB_ADDSTRING, 0, (LPARAM)name);
            break;
        }
        case S2CPacketType::RemoveUser: {
            S2CRemoveUser removeUserPacket(prd);
            char* name = g_userName[removeUserPacket.id];
            DisplayText("<< % s\r\n", name);
            int index = SendMessage(g_hListUser, LB_FINDSTRINGEXACT, 1, (LPARAM)name);
            if (index >= 0) {
                SendMessage(g_hListUser, LB_DELETESTRING, index, 0);
            }
            g_userName[removeUserPacket.id][0] = 0;
            break;
        }
        case S2CPacketType::Chat: {
            S2CChat chatPacket(prd);
            char* name = g_userName[chatPacket.id];
            DisplayText("%s: %s\r\n", name, chatPacket.message);
            break;
        }
        case S2CPacketType::ChangeName: {
            S2CChangeName namePacket(prd);
            char* oldName = g_userName[namePacket.id];
            char* newName = namePacket.name;
            DisplayText("[알림] %s님의 닉네임이 %s(으)로 변경되었습니다.\r\n", oldName, newName);
            int index = SendMessage(g_hListUser, LB_FINDSTRINGEXACT, 0, (LPARAM)oldName);
            if (index >= 0) {
                SendMessage(g_hListUser, LB_DELETESTRING, index, 0);
                SendMessage(g_hListUser, LB_INSERTSTRING, index, (LPARAM)newName);
            }
            strcpy(g_userName[namePacket.id], newName);
            if (g_id == namePacket.id) {
                SetWindowText(g_hDlg, newName);
            }
            break;
        }
        }
        delete[] buffer;
    }
    return 0;
}
// 데이터 보내기
DWORD WINAPI WriteThread(LPVOID arg) {
    int retval;
    // 서버와 데이터 통신
    while (1) {
        // 쓰기 완료 기다리기
        WaitForSingleObject(g_hWriteEvent, INFINITE);
        Packet<C2SPacketType>* sendingPacket;
        while (!g_sendQueue.isEmpty()) {
            sendingPacket = g_sendQueue.dequeue();

            switch (sendingPacket->getType()) {
            case C2SPacketType::Chat: {
                C2SChat* chatPacket = (C2SChat*)sendingPacket;
                // 문자열 길이가 0이면 보내지 않음
                if (strlen(chatPacket->message) == 0) {
                    delete sendingPacket;
                    continue;
                }
                break;
            }
            case C2SPacketType::ChangeName: // 별도 검사 없음, 패스
                break;
            default:
                continue;
            }
            // 데이터 보내기
            retval = sendingPacket->sendTo(g_sock, 0);
            if (retval == SOCKET_ERROR) {
                delete sendingPacket;
                return 1;
            }
            // 직렬화해서 넘긴 시점에 사명을 다함 ..
            delete sendingPacket;
        }
        // '메시지 전송' 버튼 활성화
        EnableWindow(g_hButtonSendMsg, TRUE);
        // 읽기 완료 알리기
        SetEvent(g_hReadEvent);
    }
    return 0;
}

// 에디트 컨트롤에 문자열 출력
void DisplayText(const char* fmt, ...) {
    va_list arg;
    va_start(arg, fmt);
    char cbuf[1024];
    vsprintf(cbuf, fmt, arg);
    EnterCriticalSection(&wcs);
    int nLength = GetWindowTextLength(g_hEditLog);
    SendMessage(g_hEditLog, EM_SETSEL, nLength, nLength);
    SendMessage(g_hEditLog, EM_REPLACESEL, FALSE, (LPARAM)cbuf);
    LeaveCriticalSection(&wcs);
    va_end(arg);
}

// 사용자 정의 데이터 수신 함수
int recvn(SOCKET s, char* buf, int len, int flags) {
    int received;
    char* ptr = buf;
    int left = len;
    while (left > 0) {
        received = recv(s, ptr, left, flags);
        if (received == SOCKET_ERROR)
            return SOCKET_ERROR;
        else if (received == 0)
            break;
        left -= received;
        ptr += received;
    }
    return (len - left);
}

// 소켓 함수 오류 출력 후 종료
void err_quit(const char* msg) {
    LPVOID lpMsgBuf;
    FormatMessage(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
        NULL, WSAGetLastError(),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPTSTR)&lpMsgBuf, 0, NULL);
    MessageBox(NULL, (LPCTSTR)lpMsgBuf, msg, MB_ICONERROR);
    LocalFree(lpMsgBuf);
    exit(1);
}

// 소켓 함수 오류 출력
void err_display(const char* msg) {
    LPVOID lpMsgBuf;
    FormatMessage(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
        NULL, WSAGetLastError(),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPTSTR)&lpMsgBuf, 0, NULL);
    DisplayText("[%s] %s", msg, (char*)lpMsgBuf);
    LocalFree(lpMsgBuf);
}
