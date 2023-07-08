#ifndef HELP
#define HELP
#include <atomic>
#include <iostream>
#include <thread>
#define PAD_UP(x, y) (((x) + (y)-1) & ~((y)-1))

template <typename T>
struct LinkList {
    T *head = nullptr;
    T *tail = nullptr;

    void insert(T *item) {
        // // inserts at head
        // T old_head = *head;
        // T old_tail = *tail;
        // T old_item = *item;

        item->prev = nullptr;
        item->next = head;
        if (head) {
            head->prev = item;
        }
        head = item;
        if (tail == nullptr) {
            tail = item;
        }
    }

    void remove(T *item) {
        if (item->prev != nullptr) {
            item->prev->next = item->next;
        }
        if (item->next != nullptr) {
            item->next->prev = item->prev;
        }
        if (item == head) {
            head = item->next;
        }
        if (item == tail) {
            tail = item->prev;
        }
        // detach item from list
        item->prev = nullptr;
        item->next = nullptr;
    }

    T *find(T *item) {
        T *curr = head;
        while (curr != nullptr) {
            if (curr == item) {
                return curr;
            }
            curr = curr->next;
        }
        return nullptr;
    }

    struct iterator {
        T *curr;
        iterator(T *curr) : curr(curr) {}
        T *operator*() { return curr; }
        iterator &operator++() {
            curr = curr->next;
            return *this;
        }
        iterator &operator--() {
            curr = curr->prev;
            return *this;
        }
        bool operator!=(const iterator &other) { return curr != other.curr; }
    };
    iterator begin() { return iterator(head); }
    iterator end() { return iterator(nullptr); }
};

class Heap;
struct SuperBlock;
#endif  // HELP
