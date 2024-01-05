/*
<개요>
- 윈도우 소켓 TCP 채팅 프로그램 서버
- 굳이 서버는 윈도우 띄울 필요 없을 것 같아서 콘솔(CUI) 기반
- 가능한지는 모르겠지만 아무튼 32(MAXUSER)명 수용 가능
- 스레드 구성은 다음과 같음
  * 메인 네트워크 스레드(main) - bind(), listen(), accept()
  * 클라이언트 스레드(CliendThread)
	* 읽기(recv) 스레드(ClientReceieveThread)
	* 쓰기(send) 스레드(ClientSendThread)

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

#include "..\Final_202113141_Packet.h"
#include "..\Final_202113141_Queue.h"

// 서버에서 갖고 있는 유저 정보 구조체
typedef struct USERINFO {
	SOCKET sock;		// 소켓
	int id;				// 아이디 - g_userList에서의 index
	char name[MAX_STRING_SIZE]; // 닉네임

	// 패킷 큐
	// 읽기 == read == sendQueue.dequeue() -> 데이터 가져오기
	// 쓰기 == write == sendQueue.enqueue() -> 데이터 추가
	Queue<Packet<S2CPacketType>*> sendQueue; 

	HANDLE recvThread; // ClientReceiveThread
	HANDLE sendThread; // ClientSendThread

	// 읽기 이벤트, 기본 활성화 상태
	// sendPacket() 호출 시 비활성화
	// sendThread에서 다 보낸 뒤에 다시 활성화됨
	// sendQueue에 한번에 한 스레드, 한 패킷만 접근 가능하도록 하는 역할
	HANDLE readEvent; 
	// 쓰기 이벤트, 기본 비활성화 상태
	// sendPacket()에서 sendQueue에 담은 뒤 활성화
	// 활성화되면 sendThread에서 받음
	// sendThread에서 sendQueue.dequeue()
	HANDLE writeEvent; 

	USERINFO(SOCKET sock, int id) : 
		sock(sock), 
		id(id),
		recvThread(nullptr),
		sendThread(nullptr)
	{
		readEvent = CreateEvent(NULL, FALSE, TRUE, NULL);
		writeEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
	}
	~USERINFO() {
		CloseHandle(readEvent);
		CloseHandle(writeEvent);

		// 힙에 남아있는 패킷 처리
		while (!sendQueue.isEmpty()) {
			delete sendQueue.dequeue();
		}
	}
	void sendPacket(Packet<S2CPacketType>* packet) {
		// 아직 뭐 받고있었으면(dequeue...) 대기
		WaitForSingleObject(readEvent, INFINITE);
		// 쓰기
		sendQueue.enqueue(packet);
		// sendThread한테 신호 주기
		SetEvent(writeEvent);
	}
	void queuePacket(Packet<S2CPacketType>* packet) {
		sendQueue.enqueue(packet);
	}
} USERINFO ;

USERINFO* g_userList[MAXUSER];
int g_userCount = 0;

DWORD WINAPI ClientThread(LPVOID arg);
DWORD WINAPI ClientReceiveThread(LPVOID arg);
DWORD WINAPI ClientSendThread(LPVOID arg);
int recvn(SOCKET s, char* buf, int len, int flags);
void err_quit(const char* msg);
void err_display(const char* msg);

int main() {
	int retval;

	// 윈속 초기화
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;

	// socket()
	SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, 0);
	if (listen_sock == INVALID_SOCKET) err_quit("socket()");


	SOCKADDR_IN serveraddr;
	ZeroMemory(&serveraddr, sizeof(serveraddr));
	serveraddr.sin_family = AF_INET;
	serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);

	// 서버 열 포트 입력
	int serverPort = 9000;
	while (1) {
		printf("[포트 입력]: ");
		retval = scanf("%d", &serverPort);
		if (retval == 0 || serverPort < 0 || serverPort >= 65536) {
			printf("[오류] 잘못된 포트를 입력했습니다.\n\n");
			continue;
		}
		// bind()
		serveraddr.sin_port = htons(serverPort);
		retval = bind(
			listen_sock,
			(SOCKADDR*)&serveraddr, sizeof(serveraddr)
		);
		if (retval == SOCKET_ERROR) {
			err_display("bind()");
			continue;
		}
		break;
	}
	printf("[포트 %d번에 서버 초기화]\n", serverPort);

	// listen()
	retval = listen(listen_sock, SOMAXCONN);
	if (retval == SOCKET_ERROR) err_quit("listen()");

	// 데이터 통신에 사용할 변수
	SOCKET client_sock;
	int addrlen, i, j;
	SOCKADDR_IN clientaddr;
	HANDLE hThread;

	// 메인 스레드는 계속 유저만 accept(), 성공 시 스레드 생성
	while (1) {
		// accept()
		addrlen = sizeof(clientaddr);
		client_sock = accept(listen_sock, (SOCKADDR*)&clientaddr, &addrlen);
		if (client_sock == INVALID_SOCKET) {
			err_display("accept()");
			continue;
		}

		// 스레드 생성
		hThread = CreateThread(NULL, 0, ClientThread, (LPVOID)client_sock, 0, NULL);
		if (hThread == nullptr) {
			closesocket(client_sock);
		} else {
			CloseHandle(hThread);
		}
	}

	return 0;
}

DWORD WINAPI ClientThread(LPVOID arg) {
	SOCKET client_sock = (SOCKET)arg;
	SOCKADDR_IN clientaddr;
	int addrlen = sizeof(clientaddr);
	getpeername(client_sock, (SOCKADDR*)&clientaddr, &addrlen);
	int retval;

	// 접속 응답 패킷 구조체
	// 성공하면 id >= 0, 실패시 id < 0
	S2CHandshake* handshake = new S2CHandshake;

	// 최대 인원 검사
	if (g_userCount >= MAXUSER) {
		printf("[오류] 최대 인원 초과!\n");
		handshake->id = SERVER_IS_FULL;
		handshake->sendTo(client_sock, 0);
		delete handshake;
		closesocket(client_sock);
		return SERVER_IS_FULL;
	}
	// 빈 자리 찾기
	int id;
	for (id = 0; id < MAXUSER; id++) {
		if (g_userList[id] == nullptr) {
			break;
		}
	}
	// 빈 자리 없으면
	if (id >= MAXUSER) {
		printf("[오류] 최대 인원 초과!\n");
		handshake->id = SERVER_IS_FULL;
		handshake->sendTo(client_sock, 0);
		delete handshake;
		closesocket(client_sock);
		return SERVER_IS_FULL;
	}

	handshake->id = id;								// 응답 패킷에 id 입력
	handshake->userCount = g_userCount;				// 응답 패킷에 현재 접속자 입력
	USERINFO* info = new USERINFO(client_sock, id); // 유저 객체 생성
	sprintf(info->name, "익명 %d", id+1);				// 닉네임 자동 설정
	info->queuePacket(handshake);			// 응답 패킷 묵히기

	// 채팅방 참가자 정보 공유
	S2CAddUser* addUserPacket, *existingUserPacket;
	for (int i = 0; i < MAXUSER; i++) {
		if (g_userList[i] == nullptr || id == i) continue; // 빈 칸/셀프 제외
		// 기존 유저한테 보내는 새로 들어온 유저 정보 패킷
		addUserPacket = new S2CAddUser(id, 0, info->name);
		g_userList[i]->sendPacket(addUserPacket);
		// 새로 들어온 유저한테 보내는 기존 유저 정보들 패킷
		existingUserPacket = new S2CAddUser(i, AU_INITIALIZING, g_userList[i]->name);
		info->queuePacket(existingUserPacket);
	}

	// 유저별 받기 / 보내기 스레드 생성
	HANDLE hThread[2];
	HANDLE recvThread = hThread[0] = CreateThread(NULL, 0, ClientReceiveThread, (LPVOID)info, 0, NULL);
	HANDLE sendThread = hThread[1] = CreateThread(NULL, 0, ClientSendThread, (LPVOID)info, 0, NULL);
	if (recvThread == nullptr || sendThread == nullptr) {
		printf("[오류] 스레드 생성 불가!\n");
		handshake->id = THREAD_ERROR;
		handshake->sendTo(client_sock, 0);
		delete handshake;
		closesocket(client_sock);
		return THREAD_ERROR;
	}
	info->recvThread = recvThread;
	info->sendThread = sendThread;

	// 리스트에 등록
	g_userList[id] = info;
	g_userCount++;
	SetEvent(info->writeEvent); // 묵힌 패킷 보내기

	// 접속한 클라이언트 정보 출력
	printf("[TCP 서버] (%02d) 클라이언트 접속 - %s:%d\n",
		id,
		inet_ntoa(clientaddr.sin_addr),
		ntohs(clientaddr.sin_port)
	);

	////////////////////////////////
	// 연결이 끊어질 때 까지 대기 //
	////////////////////////////////

	// 3번째 인자(bWaitAll)이 FALSE이므로, 하나만 끝나도 대기 끝
	retval = WaitForMultipleObjects(2, hThread, FALSE, INFINITE);
	// 반환값은 WAIT_OBJECT_0(사실상 0) ~ nCount(2) - 1
	// 즉, 제일 먼저 끝난 스레드의 인덱스 번호를 반환함
	retval -= WAIT_OBJECT_0;
	if (retval == 0) { // hThread[0]이 먼저 끝난 경우
		TerminateThread(hThread[1], 1); // 1도 같이 죽여버림
	} else { // hThread[1]이 먼저 끝난 경우
		TerminateThread(hThread[0], 1); // 0도 같이 죽여버림
	}
	// 아무튼 둘 중 하나만 먼저 죽어도, 나머지도 동반자살
	CloseHandle(hThread[0]);
	CloseHandle(hThread[1]);

	// 리스트에서 제거
	g_userList[info->id] = nullptr;
	g_userCount--;

	// 다른 클라이언트들한테 해당 유저 제거 패킷
	S2CRemoveUser* removeUserPacket;
	for (int i = 0; i < MAXUSER; i++) {
		if (g_userList[i] == nullptr) continue;
		removeUserPacket = new S2CRemoveUser(id);
		g_userList[i]->sendPacket(removeUserPacket);
	}

	// 종료 메시지
	printf("[TCP 서버] (%02d) 클라이언트 종료 - %s:%d\n",
		id,
		inet_ntoa(clientaddr.sin_addr),
		ntohs(clientaddr.sin_port)
	);
	closesocket(client_sock);
	delete info;
	return 0;
}

DWORD WINAPI ClientReceiveThread(LPVOID arg) {
	USERINFO* info = (USERINFO*)arg;
	int retval;
	int type, size;
	char* buffer;
	C2SPacketType typeEnum;
	PacketReceiveData prd;
	while (1) {
		// 패킷 종류 받아오기 (4bytes)
		retval = recvn(info->sock, (char*)&type, sizeof(type), 0);
		if (retval == SOCKET_ERROR) { // 오류 발생하면
			err_display("recvn()"); // 출력
			break;
		} else if (retval == 0) { // 연결 종료 시
			break;
		}
		if (type <= (int)C2SPacketType::_Start // 유효한 패킷 종류 범위가 아님
			|| type >= (int)C2SPacketType::_End
		) {
			printf("[TCP 서버] 오류: 유효하지 않은 패킷 종류: %d\n", type);
			break;
		}
		typeEnum = (C2SPacketType)type;
		// 패킷 데이터 크기 받아오기 (4bytes)
		retval = recvn(info->sock, (char*)&size, sizeof(size), 0);
		if (retval == SOCKET_ERROR) { // 오류 발생하면
			err_display("recvn()"); // 출력
			break;
		} else if (retval == 0) { // 연결 종료 시
			break;
		}
		if (size <= 0				// 최소/최대 패킷 범위 넘어감
			|| size >= MAX_PACKET_SIZE
		) {
			printf("[TCP 서버] 오류: 유효하지 않은 패킷 크기: %d\n", type);
			break;
		}
		// 받아온 패킷 데이터 크기만큼 받아오기
		buffer = new char[size];
		// 1. 버퍼에 받아오고
		retval = recvn(info->sock, buffer, size, 0);
		if (retval == SOCKET_ERROR) { // 오류 발생하면
			err_display("recvn()"); // 출력
			break;
		} else if (retval == 0) { // 연결 종료 시
			break;
		}
		// 2. PacketReceiveData { int, int, const char* } 에 받아온 데이터 넣어오고
		prd = { type, size, buffer };

		printf("[TCP 서버] [recv] %s(%02d) 로부터 %d바이트 데이터 받음: {type=%d, size=%d}\n",
			info->name,
			info->id,
			size + PACKET_HEADER_SIZE,
			type,
			size
		);

		switch (typeEnum) { // 패킷 종류에 따라 판단
		case C2SPacketType::ChangeName: {
			// 이름 변경 패킷
			// 3. 각 클래스에서 역직렬화
			C2SChangeName changeName(prd);
			char* oldName = g_userList[info->id]->name;
			char* newName = changeName.name;
			printf("[TCP 서버] (%02d) 닉네임 변경: %s -> %s\n", info->id, oldName, newName);
			strcpy(g_userList[info->id]->name, newName);

			S2CChangeName* sendingChangeName;
			for (int i = 0; i < MAXUSER; i++) {
				if (g_userList[i] == nullptr) continue;
				sendingChangeName = new S2CChangeName(info->id, newName);
				g_userList[i]->sendPacket(sendingChangeName);
			}
			break;
		}
		case C2SPacketType::Chat: {
			// 채팅 - 모든 유저에게(자신 포함) 채팅 패킷 보내기
			C2SChat chat(prd);
			printf("[TCP 서버] (%02d) %s: %s\n", info->id, g_userList[info->id]->name, chat.message);

			S2CChat* sendingChat;
			for (int i = 0; i < MAXUSER; i++) {
				if (g_userList[i] == nullptr) continue;
				sendingChat = new S2CChat(info->id, chat.message);
				g_userList[i]->sendPacket(sendingChat);
			}
			break;
		}
		}
		delete[] buffer;
	}
	return 0;
}

DWORD WINAPI ClientSendThread(LPVOID arg) {
	USERINFO* info = (USERINFO*)arg;
	int retval;
	Packet<S2CPacketType>* sendingPacket;
	while (1) {
		// 보내기 신호 오기까지 기다림
		WaitForSingleObject(info->writeEvent, INFINITE);
		if (info->sendQueue.isEmpty()) {
			SetEvent(info->readEvent);
			continue;
		}
		while (!info->sendQueue.isEmpty()) {
			sendingPacket = info->sendQueue.dequeue();
			printf("[TCP 서버] [send] %s(%02d) 에 %d바이트 보냄: {type=%d, size=%d}\n",
				info->name,
				info->id,
				sendingPacket->getTotalSize(),
				sendingPacket->getType(),
				sendingPacket->getSize()
			);
			retval = sendingPacket->sendTo(info->sock, 0);
			if (retval == SOCKET_ERROR) {
				err_display("send()");
				delete sendingPacket;
				return 1;
			}
			delete sendingPacket;
		}
		SetEvent(info->readEvent);
	}
	return 0;
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
	printf("[%s] %s", msg, (char*)lpMsgBuf);
	LocalFree(lpMsgBuf);
}