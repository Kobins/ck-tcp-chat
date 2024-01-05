/*
<참고·참조한 자료들>
Linked List를 활용한 큐: 게임자료구조 시간때 배웠던 형태 차용
*/

#pragma once

template <typename T>
class Node {
public:
	Node<T>* next;
	T data;
	Node(T data, Node<T>* next = nullptr) : data(data), next(next) {}
};
template <typename T>
class Queue {
	Node<T>* head;
	Node<T>* tail;
	int size;
public:
	Queue() : head(nullptr), tail(nullptr), size(0) {}
	~Queue() {
		Node<T>* temp;
		while (head) {
			temp = head->next;
			delete head;
			head = temp;
		}
		if (tail) {
			delete tail;
		}
	}
	void enqueue(T data) {
		Node<T>* newNode = new Node<T>(data);
		if (head == nullptr) {
			head = newNode;
		} else {
			tail->next = newNode;
		}
		tail = newNode;
		size++;
	}
	T dequeue() {
		if (isEmpty()) {
			exit(1);
		}
		Node<T>* node = head;
		T data = (head->data);
		head = head->next;
		delete node;
		size--;
		if (size <= 0) {
			tail = nullptr;
		}
		return data;
	}
	T getTop() {
		if (isEmpty()) {
			return exit(1);
		}
		return head->data;
	}
	bool isEmpty() const { return size <= 0; }
};