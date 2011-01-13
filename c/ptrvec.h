#ifndef PTRVEC_H__
#define PTRVEC_H__

template <typename T>
class PtrVec {
    static const int INTERNAL_BUF_CAP = 16;
    int len;
    int cap;
    T * els;
    T * buf[INTERNAL_BUF_CAP];

public:
    void EnsureCap(int needed) {
        if (this->cap > needed)
            return;
        int newcap;
        if (this->cap < 1024)
            newcap = this->cap * 2;
        else
            newcap = this->cap * 3 / 2;
        if (needed > newcap)
            newcap = needed;

        T * newels = (T*)malloc(newcap * sizeof(T*));
        memcpy(newels, els, newcap * sizeof(T*));
        if (els != buf)
            free(els);
        els = newels;
        cap = newcap;
    }

    PtrVec(int initcap=0) {
        els = buf;
        cap = INTERNAL_BUF_CAP;
        len = 0;
        EnsureCap(initcap);
    }

    ~PtrVec() {
        if (els != buf)
            free(els);
    }

    T* At(int idx) {
        return els[idx];
    }

    int Count() {
        return len;
    }

    void Append(T *el) {
        EnsureCap(len + 1);
        els[len++] = el;
    }

    void InsertAt(int idx, T *el) {
        EnsureCap(len + 1);
        int tomove = len - idx;
        if (tomove > 0) {
            T *src = els + idx;
            T *dst = els + idx + 1;
            memmove(dst, src, tomove * sizeof(T*));
        }
        els[idx] = el;
        ++len;
    }

    void RemoveAt(int idx, int count=1) {
        int tomove = len - idx - count;
        if (tomove > 0) {
            T *dst = els + idx;
            T *src = els + idx + count;
            memmove(dst, src, tomove * sizeof(T*));
        }
        len -= count;
    }

    void Push(T *el) {
        Append(el);
    }

    T* Pop() {
        if (0 == len)
            return NULL;
        return els[--len];
    }

    void Clear() {
        len = 0;
        if (els != buf)
            free(els);
        els = buf;
        cap = INTERNAL_BUF_CAP;            
    }

    void DeleteAll() {
        while (len > 0) {
            T *el = Pop();
            delete el;
        }
    }
};

#endif
