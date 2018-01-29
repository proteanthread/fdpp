#ifndef FARPTR_HPP
#define FARPTR_HPP

#include <type_traits>
#include <cassert>
#include "thunks_priv.h"
#include "dosobj_priv.h"

void store_far(void *ptr, far_s fptr);
far_s lookup_far(void *ptr);

#define _P(T1) std::is_pointer<T1>::value
#define _C(T1) std::is_const<T1>::value
#define _RP(T1) typename std::remove_pointer<T1>::type
template<typename T> class SymWrp;
template<typename T> class SymWrp2;
template<typename T>
class FarPtr : protected far_s {
public:
    FarPtr() = default;
    FarPtr(uint16_t s, uint16_t o) : far_s((far_s){s, o}) {}
    FarPtr(std::nullptr_t) : far_s((far_s){0, 0}) {}
#define ALLOW_CNV0(T0, T1) std::is_convertible<T0*, T1*>::value
#define ALLOW_CNV1(T0, T1) \
        std::is_void<T0>::value || std::is_same<T0, char>::value || \
        std::is_same<T1, char>::value || \
        std::is_same<T0, unsigned char>::value || \
        std::is_same<T1, unsigned char>::value
#define ALLOW_CNV(T0, T1) (ALLOW_CNV0(T0, T1) || ALLOW_CNV1(T0, T1))
    template<typename T0, typename T1 = T,
        typename std::enable_if<ALLOW_CNV(T0, T1)>::type* = nullptr>
    FarPtr(const FarPtr<T0>& f) : far_s((far_s){f.__seg(), f.__off()}) {}

    template<typename T0, typename T1 = T,
        typename std::enable_if<!ALLOW_CNV(T0, T1)>::type* = nullptr>
    explicit FarPtr(const FarPtr<T0>& f) : far_s((far_s){f.__seg(), f.__off()}) {}

    T* operator ->() { return (T*)resolve_segoff(*this); }
    operator T*() { return (T*)resolve_segoff(*this); }

    template <typename T1 = T,
        typename std::enable_if<std::is_class<T1>::value>::type* = nullptr>
    SymWrp<T1>& get_wrp() {
        SymWrp<T1> *s = (SymWrp<T1> *)get_ptr();
        store_far(s, get_far());
        return *s;
    }
    template <typename T1 = T,
        typename std::enable_if<std::is_class<T1>::value>::type* = nullptr>
    SymWrp<T1>& operator [](unsigned idx) {
        return FarPtr<T1>(*this + idx).get_wrp();
    }
    template <typename T1 = T,
        typename std::enable_if<!std::is_class<T1>::value>::type* = nullptr>
    SymWrp2<T1>& get_wrp() {
        SymWrp2<T1> *s = (SymWrp2<T1> *)get_ptr();
        store_far(s, get_far());
        return *s;
    }
    template <typename T1 = T,
        typename std::enable_if<!std::is_class<T1>::value>::type* = nullptr>
    SymWrp2<T1>& operator [](unsigned idx) {
        return FarPtr<T1>(*this + idx).get_wrp();
    }

    template<typename T0, typename T1 = T,
        typename std::enable_if<ALLOW_CNV1(T1, T0)>::type* = nullptr>
    explicit operator T0*() { return (T0*)resolve_segoff(*this); }

    FarPtr<T> operator ++(int) {
        FarPtr<T> f = *this;
        off += sizeof(T);
        return f;
    }
    FarPtr<T> operator ++() {
        off += sizeof(T);
        return *this;
    }
    FarPtr<T> operator --() {
        off -= sizeof(T);
        return *this;
    }
    void operator +=(int inc) { off += inc * sizeof(T); }
    FarPtr<T> operator +(int inc) { return FarPtr<T>(seg, off + inc * sizeof(T)); }
    FarPtr<T> operator -(int dec) { return FarPtr<T>(seg, off - dec * sizeof(T)); }
    uint16_t __seg() const { return seg; }
    uint16_t __off() const { return off; }
    uint32_t get_fp32() const { return ((seg << 16) | off); }
    far_s get_far() const { return *this; }
    far_s& get_ref() { return *this; }
    T* get_ptr() { return (T*)resolve_segoff(*this); }
};

#define _MK_F(f, s) ({ far_s __s = s; f(__s.seg, __s.off); })

/* These SymWrp are tricky, and are needed only because we
 * can't provide 'operator.':
 * https://isocpp.org/blog/2016/02/a-bit-of-background-for-the-operator-dot-proposal-bjarne-stroustrup
 * Consider the following code (1):
 * void FAR *f = &fp[idx];
 * In this case & should create "void FAR *" from ref, not just "void *".
 * Wrapper helps with this.
 * And some other code (2) does this:
 * int a = fp[idx].a_memb;
 * in which case we should better have no wrapper.
 * Getting both cases to work together is challenging.
 * Note that the simplest solution "operator T &()" for case 2
 * currently doesn't work, but it may work in the future:
 * http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2017/p0352r1.pdf
 * But I don't think waiting for this document to materialize in
 * gcc/g++ is a good idea, as it is not even a part of the upcoming C++20.
 * So what I do is a public inheritance of T. This kind of works,
 * but puts an additional restrictions on the wrapper, namely, that
 * it should be a POD, it should not add any data members and it should
 * be non-copyable.
 * Fortunately the POD requirement is satisfied with 'ctor = default'
 * trick, and non-copyable is simple, but having no data members means
 * I can't easily retrieve the far ptr for case 1.
 * So the only solution I can come up with, is to put the static
 * map somewhere that will allow to look up the "lost" far pointers.
 * Thus farhlp.cpp
 */
template<typename T>
class SymWrp : public T {
public:
    SymWrp() = default;
    SymWrp(const SymWrp&) = delete;
    SymWrp<T>& operator =(T& f) { *this = f; return *this; }
    FarPtr<T> operator &() { return _MK_F(FarPtr<T>, lookup_far(this)); }
};

template<typename T>
class SymWrp2 {
    T sym;

public:
    SymWrp2() = default;
    SymWrp2(const SymWrp2&) = delete;
    SymWrp2<T>& operator =(const T& f) { sym = f; return *this; }
    FarPtr<T> operator &() { return _MK_F(FarPtr<T>, lookup_far(this)); }
    operator T &() { return sym; }
    /* for fmemcpy() etc that need const void* */
    template <typename T1 = T,
        typename std::enable_if<_P(T1) &&
        !std::is_void<_RP(T1)>::value>::type* = nullptr>
    operator FarPtr<const void> () {
        return _MK_F(FarPtr<const void>, lookup_far(this));
    }
};

template<typename T> class AsmSym;
template<typename T>
class AsmRef {
    FarPtr<T> *sym;

public:
    AsmRef(FarPtr<T> *s) : sym(s) {}
    T* operator ->() { return *sym; }
    operator FarPtr<T> () { return *sym; }
    template <typename T1 = T,
        typename std::enable_if<!std::is_void<T1>::value>::type* = nullptr>
    operator FarPtr<void> () { return FarPtr<void>(*sym); }
    uint16_t __seg() const { return sym->__seg(); }
    uint16_t __off() const { return sym->__off(); }
};

template<typename T>
class AsmSym {
    FarPtr<T> sym;

public:
    template <typename T1 = T,
        typename std::enable_if<std::is_class<T1>::value>::type* = nullptr>
    SymWrp<T1>& get_sym() { return sym.get_wrp(); }
    template <typename T1 = T,
        typename std::enable_if<!std::is_class<T1>::value>::type* = nullptr>
    SymWrp2<T1>& get_sym() { return sym.get_wrp(); }
    AsmRef<T> operator &() { return AsmRef<T>(&sym); }

    /* everyone with get_ref() method should have no copy ctor */
    AsmSym() = default;
    AsmSym(const AsmSym<T> &) = delete;
    far_s* get_ref() { return &sym.get_ref(); }
};

template<typename T>
class AsmFSym {
    FarPtr<T> sym;

public:
    FarPtr<T> get_sym() { return sym; }

    AsmFSym() = default;
    AsmFSym(const AsmFSym<T> &) = delete;
    far_s* get_ref() { return &sym.get_ref(); }
};

template<typename T>
class CallSym {
    FarPtr<T> ptr;

public:
    CallSym(const FarPtr<T>& f) { ptr = f; }
    template <typename T1 = T,
        typename std::enable_if<std::is_void<T1>::value>::type* = nullptr>
    T1 operator()() { thunk_call_void(ptr.get_far()); }
    template <typename T1 = T,
        typename std::enable_if<!std::is_void<T1>::value>::type* = nullptr>
    T1 operator()() { return thunk_call_void(ptr.get_far()); }
};

template<typename T>
class AsmCSym {
    CallSym<T> sym;

public:
    AsmCSym(const FarPtr<T>& f) : sym(f) {}
    CallSym<T>& operator *() { return sym; }
};

template<typename T>
class NearPtr {
    uint16_t off;

public:
    explicit NearPtr(uint16_t o) : off(o) {}    // for farobj only
    NearPtr(std::nullptr_t) : off(0) {}
    operator uint16_t () { return off; }
    operator T *() { return FarPtr<T>(dosobj_seg(), off); }
    NearPtr<T> operator - (const NearPtr<T>& n) const {
        return NearPtr<T>(off - n.__off());
    }
    uint16_t __off() const { return off; }

    NearPtr() = default;
};

template<typename T, int max_len = 0>
class ArSymBase {
    FarPtr<T> sym;

public:
    template <typename T1 = T,
        typename std::enable_if<std::is_class<T1>::value>::type* = nullptr>
    SymWrp<T1>& operator [](unsigned idx) {
        assert(!max_len || idx < max_len);
        return sym[idx];
    }

    template <typename T1 = T,
        typename std::enable_if<!std::is_class<T1>::value>::type* = nullptr>
    SymWrp2<T1>& operator [](unsigned idx) {
        assert(!max_len || idx < max_len);
        return sym[idx];
    }
};

template<typename T, int max_len = 0>
class ArSym : public ArSymBase<T> {
public:
    ArSym(std::nullptr_t);
    ArSym(const FarPtr<void> &);
    operator FarPtr<void> ();
    template <typename T1 = T,
        typename std::enable_if<!_C(T1)>::type* = nullptr>
        operator FarPtr<const void> ();
    template <typename T1 = T,
        typename std::enable_if<!_C(T1)>::type* = nullptr>
        operator FarPtr<const T1> ();
    operator FarPtr<T> ();
    operator NearPtr<T> ();
    operator T *();
    template <typename T0, typename T1 = T,
        typename std::enable_if<!std::is_same<T0, T1>::value>::type* = nullptr>
        explicit operator T0 *();
    FarPtr<T> operator +(int);
    FarPtr<T> operator +(unsigned);
    FarPtr<T> operator +(size_t);
    FarPtr<T> operator -(int);

    ArSym() = default;
    ArSym(const ArSym<T> &) = delete;
};

template<typename T, int max_len = 0>
class AsmArSym : public ArSymBase<T> {
public:
    AsmArSym() = default;
    AsmArSym(const AsmArSym<T> &) = delete;
    far_s* get_ref();
};

template<typename T, int max_len = 0>
class AsmArNSym : public AsmArSym<T> {
public:
    NearPtr<T> get_sym();

    AsmArNSym() = default;
    AsmArNSym(const AsmArNSym<T> &) = delete;
};

template<typename T, int max_len = 0>
class AsmArFSym : public AsmArSym<T> {
public:
    FarPtr<T> get_sym();

    AsmArFSym() = default;
    AsmArFSym(const AsmArFSym<T> &) = delete;
};

template<typename T>
class FarPtrAsm {
    FarPtr<FarPtr<T>> ptr;

public:
    explicit FarPtrAsm(const FarPtr<FarPtr<T>>& f) : ptr(f) {}
    /* some apps do the following: *(UWORD *)&f_ptr = new_offs; */
    explicit operator uint16_t *() { return &ptr->get_ref().off; }
    uint16_t __seg() const { return ptr.__seg(); }
    uint16_t __off() const { return ptr.__off(); }
};

template<typename T>
class AsmFarPtr {
    FarPtr<FarPtr<T>> ptr;

public:
    FarPtr<T>& get_sym() { return *ptr.get_ptr(); }
    FarPtrAsm<T> operator &() { return FarPtrAsm<T>(ptr); }

    AsmFarPtr() = default;
    AsmFarPtr(const AsmFarPtr<T> &) = delete;
    far_s* get_ref();
};

#undef _P
#undef _C
#undef _RP

#define __ASMSYM(t) AsmSym<t>
#define __ASMFSYM(t) AsmFSym<t>
#define __ASMARSYM(t, v, l) AsmArFSym<t, l> v
#define __ASMARISYM(t, v) AsmArNSym<t> v
#define __ASMARIFSYM(t, v) AsmArFSym<t> v
#define __FAR(t) FarPtr<t>
#define __ASMFAR(t) AsmFarPtr<t>
#define __ASMREF(f) f.get_ref()
#define __ASMADDR(v) &__##v
#define __ASMCALL(t, f) AsmCSym<t> f
#define __ASYM(x) x.get_sym()
#define ASMREF(t) AsmRef<t>
#define AR_MEMB(t, n, l) ArSym<t, l> n
#define SYM_MEMB(t) SymWrp<t>
#define SYM_MEMB_T(t) SymWrp2<t>
#define PTR_MEMB(t) NearPtr<t>
#define FP_SEG(fp)            ((fp).__seg())
#define FP_OFF(fp)            ((fp).__off())
#define MK_FP(seg,ofs)        (__FAR(void)(seg, ofs))
#define __DOSFAR(t) FarPtr<t>
#define _MK_DOS_FP(t, s, o) __FAR(t)MK_FP(s, o)
#define GET_FP32(f) f.get_fp32()
#define GET_FAR(f) f.get_far()

#undef NULL
#define NULL           nullptr

#endif
