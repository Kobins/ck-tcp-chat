/*
<개요>
- 패킷 네트워크 통신을 위해 필요한 여러가지 정의
- BufferWriter, BufferReader: 직렬화된 char 배열에서 원하는 자료형으로 긁어오기 편하게 만든 클래스
- PacketReceiveData: 패킷 recv & 역직렬화 시 사용하는 구조체
- S2CPacketType: Server To Client 인 패킷 종류 열거형
- C2SPacketType: Client To Server 인 패킷 종류 열거형
- Packet<T>: 패킷 추상 클래스, 종류와 데이터 크기를 담고 있음
- S2C*: Server To Client인 패킷 클래스, Packet<S2CPacketType> 상속
- C2S*: Client To Server인 패킷 클래스, Packet<C2SPacketType> 상속

<참고·참조한 자료들>
- 패킷 클래스: 14주차 예제 패킷 구조(COMM_MSG, CHAT_MSG, DRAW_MSG ...)에서 강화
- BufferWriter, BufferReader: Java의 ByteOutputStream, ByteInputStream을 사용하던 기억에서 차용
*/

#pragma once

#define MAXUSER 32 // 최대 인원수 (이론상)
#define PACKET_HEADER_SIZE (sizeof(int) * 2) // 패킷 헤더 크기(종류, 크기 8바이트)
#define MAX_PACKET_SIZE 512 // 최대 패킷 크기
#define MAX_STRING_SIZE 256 // 최대 문자열 크기

#include <winsock2.h>

// char 배열에 순서대로 원하는 타입의 데이터를 쓸 수 있음
// - int(4), int(4), char(1) 으로 이루어진 9바이트 char 배열을 만드려면
//   write(int), write(int), write(char)로 쓸 수 있음
// - 내부에서 char 배열을 생성하지 않음, 따라서 미리 배열을 만들어서 넣어야 함
class BufferWriter {
	char* buf;
	int cur;
public:
	BufferWriter(char* buf) : buf(buf), cur(0) {}
	~BufferWriter() { buf = nullptr; }
	template <typename T>
	void write(const T& value) {
		// buf -> 1바이트 char 배열
		// buf + cur -> buf에서 cur바이트 뒤로
		// (T*)(buf + cur) -> buf에서 cur바이트 뒤에서부터 sizeof(T)만큼 읽어오는 포인터 자료형으로 변환
		// *((T*)(buf + cur)) -> sizeof(T)만큼으로 강제 형변환
		*((T*)(buf + cur)) = value;
		cur += sizeof(T);
	}
	// 문자열(char* 배열)은 별도로 처리 (null-terminated 문자열)
	void writeString(const char* value) {
		int length = strlen(value) + 1;
		strcpy_s(buf + cur, length, value);
		cur += length;
	}
	char* getBuffer() { return buf; }
};

// BufferReader의 반대 역할
// const char 배열으로부터 순서대로 원하는 타입으로 읽어옴
// - int(4), int(4), char(1) 으로 이루어진 9바이트 char 배열인 경우
//   read<int>(), read<int>(), read<char>()와 같이 가져올 수 있음.
class BufferReader {
	const char* buf;
	int cur;
public:
	BufferReader(const char* buf) : buf(buf), cur(0) {}
	~BufferReader() { buf = nullptr; }
	template <typename T>
	const T& read() {
		// buf -> 1바이트 char 배열
		// buf + cur -> buf에서 cur바이트 뒤로
		// (T*)(buf + cur) -> buf에서 cur바이트 뒤에서부터 sizeof(T)만큼 읽어오는 포인터 자료형으로 변환
		// *((T*)(buf + cur)) -> sizeof(T)만큼으로 강제 형변환
		const T& value = *((T*)(buf + cur));
		cur += sizeof(T);
		return value;
	}
	// 문자열(char* 배열)은 별도로 처리 (null-terminated 문자열)
	const char* readString() {
		const char* value = (buf + cur);
		int length = strlen(value) + 1;
		cur += length;
		return value;
	}
	const char* getBuffer() { return buf; }
};
// 소켓으로 recv할 때 사용하는 구조체
typedef struct {
	int type;
	int size;
	const char* buffer;
} PacketReceiveData;

// Server to Client
enum class S2CPacketType : int {
	_Start = 0x00,
	Handshake,
	AddUser,
	RemoveUser,
	Chat,
	ChangeName,
	_End
};

// Client to Server
enum class C2SPacketType : int {
	_Start = 0x00,
	Chat,
	ChangeName,
	_End,
};

template <typename T>
class Packet {
protected:
	T type;		// 패킷 종류
	int size;	// 패킷 데이터 크기

	char* buf;	// 직렬화용 버퍼
public:
	Packet(T type, int size = 0) : type(type), size(size) {
		// 직렬화용 버퍼의 초기화
		if (size != 0)
			buf = new char[getTotalSize()]; // size + sizeof(T=int) + sizeof(int)
		else
			// late init size
			// 생성자 이니셜라이저로 바로 size를 초기화할 수 없으면
			// size를 바로 넣지 않고, setSize(int) 사용
			buf = nullptr; 
	}
	virtual ~Packet() {
		if (buf) {
			delete[] buf;
			buf = nullptr;
		}
	}
	T getType() const { return type; }
	int getSize() const { return size; }
	void setSize(int value) {
		// 기존에 생성된 크기 다른 버퍼가 있으면 지우기
		if (buf && size != value) {
			delete[] buf;
			buf = nullptr;
		}
		size = value;
		buf = new char[getTotalSize()];
	}
	int getTotalSize() const { return size + PACKET_HEADER_SIZE; }
	// 직렬화 함수
	const char* serialize() {
		BufferWriter out(buf);
		// char* 배열로 형변환할 때에는 보낼 용도이므로 type과 size 포함
		out.write(type);
		out.write(size);
		writeToBuffer(out);
		return buf;
	}
	virtual void writeToBuffer(BufferWriter& out) abstract;
	// 역직렬화 생성자
	Packet(const PacketReceiveData& recvData) : type((T)recvData.type), size(recvData.size), buf(nullptr) {
		// 여기서 순수 가상함수(readFromBuffer)를 호출하고 싶었지만 . . .
		// 부모 생성자 시점에서는 자식이 아직 없으므로 호출할 수 없다
		// 그래서 자식 클래스마다 readFromBuffer(recvData)를 호출하는 생성자를 만듦
	}
	void readFromBuffer(const PacketReceiveData& recvData) {
		BufferReader in(recvData.buffer);
		readFromBuffer(in);
	}
	virtual void readFromBuffer(BufferReader& in) abstract;
	// 패킷 편하게 보내기
	int sendTo(SOCKET sock, int flags) {
		return send(sock, serialize(), getTotalSize(), flags);
	}
};

// 채팅 요청
class C2SChat : public Packet<C2SPacketType> {
public:
	int messageSize;
	char* message = nullptr;

	C2SChat(
		const char* message
	) :
		Packet<C2SPacketType>(C2SPacketType::Chat),
		messageSize(strlen(message) + 1)
	{
		this->message = new char[messageSize];
		strcpy_s(this->message, messageSize, message);
		setSize(sizeof(int) + messageSize);
	}
	C2SChat(const PacketReceiveData& recvData) : Packet<C2SPacketType>(recvData) { Packet::readFromBuffer(recvData); }
	~C2SChat() override {
		if (message) {
			delete[] message;
			message = nullptr;
		}
	}
	virtual void writeToBuffer(BufferWriter& out) override {
		out.write(messageSize);
		out.writeString(message);
	}
	virtual void readFromBuffer(BufferReader& in) override {
		messageSize = in.read<int>();
		if (message == nullptr) {
			message = new char[messageSize];
		}
		strcpy_s(message, messageSize, in.readString());
	}
};

// 닉네임 변경 요청
class C2SChangeName : public Packet<C2SPacketType> {
public:
	int nameSize;
	char* name = nullptr;

	C2SChangeName(
		const char* name
	) :
		Packet<C2SPacketType>(C2SPacketType::ChangeName, sizeof(int) + nameSize),
		nameSize(strlen(name) + 1)
	{
		this->name = new char[nameSize];
		strcpy_s(this->name, nameSize, name);
		setSize(sizeof(int) + nameSize);
	}
	C2SChangeName(const PacketReceiveData& recvData) : Packet<C2SPacketType>(recvData) { Packet::readFromBuffer(recvData); }
	~C2SChangeName() override {
		if (name) {
			delete[] name;
			name = nullptr;
		}
	}
	virtual void writeToBuffer(BufferWriter& out) override {
		out.write(nameSize);
		out.writeString(name);
	}
	virtual void readFromBuffer(BufferReader& in) override {
		nameSize = in.read<int>();
		if (name == nullptr) {
			name = new char[nameSize];
		}
		strcpy_s(name, nameSize, in.readString());
	}
};

////////////////////////////////////////////


// 접속 응답 (배정된 id, 기존 유저 수)
class S2CHandshake : public Packet<S2CPacketType> {
public:
	int id;
	int userCount;

	S2CHandshake(
		int id = -1,
		int userCount = 0
	) :
		Packet<S2CPacketType>(S2CPacketType::Handshake, sizeof(int) * 2),
		id(id),
		userCount(userCount)
	{}
	S2CHandshake(const PacketReceiveData& recvData) : Packet<S2CPacketType>(recvData) { Packet::readFromBuffer(recvData); }
	virtual void writeToBuffer(BufferWriter& out) override {
		out.write(id);
		out.write(userCount);
	}
	virtual void readFromBuffer(BufferReader& in) override {
		id = in.read<int>();
		userCount = in.read<int>();
	}
};

// 응답 시 에러 코드
enum S2CHandshakeErrorCode {
	FAILED_TO_CONNECT = -1,
	SERVER_IS_FULL = -2,
	THREAD_ERROR = -3,
};

// 다른 유저 접속
class S2CAddUser : public Packet<S2CPacketType> {
public:
	int id;
	int flag;
	int nameSize;
	char* name = nullptr;

	S2CAddUser(
		int id,
		int flag,
		const char* name
	) :
		Packet<S2CPacketType>(S2CPacketType::AddUser),
		id(id),
		flag(flag),
		nameSize(strlen(name) + 1)
	{
		this->name = new char[nameSize];
		strcpy_s(this->name, nameSize, name);
		setSize(sizeof(int) * 3 + nameSize);
	}
	S2CAddUser(const PacketReceiveData& recvData) : Packet<S2CPacketType>(recvData) { Packet::readFromBuffer(recvData); }
	~S2CAddUser() override {
		if (name) {
			delete[] name;
			name = nullptr;
		}
	}
	virtual void writeToBuffer(BufferWriter& out) override {
		out.write(id);
		out.write(flag);
		out.write(nameSize);
		out.writeString(name);
	}
	virtual void readFromBuffer(BufferReader& in) override {
		id = in.read<int>();
		flag = in.read<int>();
		nameSize = in.read<int>();
		if (name == nullptr) {
			name = new char[nameSize];
		}
		strcpy_s(name, nameSize, in.readString());
	}
};

enum S2CAddUserFlag {
	// 접속 성공 시 보내주는 현재 참가자 패킷(메시지 출력 안 함)
	AU_INITIALIZING = 0x01, 
};

// 다른 유저 퇴장
class S2CRemoveUser : public Packet<S2CPacketType> {
public:
	int id;

	S2CRemoveUser(
		int id
	) :
		Packet<S2CPacketType>(S2CPacketType::RemoveUser, sizeof(int)),
		id(id)
	{}
	S2CRemoveUser(const PacketReceiveData& recvData) : Packet<S2CPacketType>(recvData) { Packet::readFromBuffer(recvData); }
	virtual void writeToBuffer(BufferWriter& out) override {
		out.write(id);
	}
	virtual void readFromBuffer(BufferReader& in) override {
		id = in.read<int>();
	}
};

// 다른 유저 채팅
class S2CChat : public Packet<S2CPacketType> {
public:
	int id;
	int messageSize;
	char* message = nullptr;

	S2CChat(
		int id,
		const char* message
	) :
		Packet<S2CPacketType>(S2CPacketType::Chat),
		id(id),
		messageSize(strlen(message) + 1)
	{
		this->message = new char[messageSize];
		strcpy_s(this->message, messageSize, message);
		setSize(sizeof(int) * 2 + messageSize); // late init
	}
	S2CChat(const PacketReceiveData& recvData) : Packet<S2CPacketType>(recvData) { Packet::readFromBuffer(recvData); }
	~S2CChat() override {
		if (message) {
			delete[] message;
			message = nullptr;
		}
	}
	virtual void writeToBuffer(BufferWriter& out) override {
		out.write(id);
		out.write(messageSize);
		out.writeString(message);
	}
	virtual void readFromBuffer(BufferReader& in) override {
		id = in.read<int>();
		messageSize = in.read<int>();
		if (message == nullptr) {
			message = new char[messageSize];
		}
		strcpy_s(message, messageSize, in.readString());
	}
};

// 다른 유저 이름 변경
class S2CChangeName : public Packet<S2CPacketType> {
public:
	int id;
	int nameSize;
	char* name = nullptr;

	S2CChangeName(
		int id,
		const char* name
	) :
		Packet<S2CPacketType>(S2CPacketType::ChangeName),
		id(id),
		nameSize(strlen(name) + 1)
	{
		this->name = new char[nameSize];
		strcpy_s(this->name, nameSize, name);
		setSize(sizeof(int) * 2 + nameSize);
	}
	S2CChangeName(const PacketReceiveData& recvData) : Packet<S2CPacketType>(recvData) { Packet::readFromBuffer(recvData); }
	virtual void writeToBuffer(BufferWriter& out) override {
		out.write(id);
		out.write(nameSize);
		out.writeString(name);
	}
	virtual void readFromBuffer(BufferReader& in) override {
		id = in.read<int>();
		nameSize = in.read<int>();
		if (name == nullptr) {
			name = new char[nameSize];
		}
		strcpy_s(name, nameSize, in.readString());
	}
};