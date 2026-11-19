#pragma once

#include <tower/result.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <tuple>
#include <typeindex>
#include <unordered_map>

namespace tower {

enum class errc : std::uint8_t { missing_dependency = 1, duplicate_dependency };

namespace detail {

class DependencyErrorCategory : public std::error_category {
public:
    const char* name() const noexcept override { return "tower"; }

    std::string message(int value) const override {
        switch (value) {
        case static_cast<int>(errc::missing_dependency): return "missing dependency";
        case static_cast<int>(errc::duplicate_dependency): return "duplicate dependency";
        default: return "unknown tower error";
        }
    }
};

template<class T>
using dependency_t = std::remove_cv_t<std::remove_reference_t<T>>;

template<std::size_t Alignment, bool OverAligned = (Alignment > alignof(std::max_align_t))>
struct DependencyAllocation {};

// C++14's global operator new only guarantees fundamental alignment.
template<std::size_t Alignment>
struct DependencyAllocation<Alignment, true> {
    static void* operator new(std::size_t size) {
        std::size_t space = size + Alignment - 1;
        auto base = static_cast<unsigned char*>(::operator new(space + sizeof(unsigned char*)));
        void* aligned = base + sizeof(base);
        std::align(Alignment, size, aligned, space);
        std::memcpy(static_cast<unsigned char*>(aligned) - sizeof(base),
                    static_cast<const void*>(&base), sizeof(base));
        return aligned;
    }

    static void operator delete(void* aligned) noexcept {
        unsigned char* base = nullptr;
        std::memcpy(static_cast<void*>(&base),
                    static_cast<unsigned char*>(aligned) - sizeof(base), sizeof(base));
        ::operator delete(base);
    }
};

template<class T, bool IsArray = std::is_array<T>::value>
struct DependencyEntry : DependencyAllocation<alignof(T)> {
    template<class... Args, std::enable_if_t<std::is_constructible<T, Args&&...>::value, int> = 0>
    explicit DependencyEntry(Args&&... args) : value(std::forward<Args>(args)...) {}

    T value;
};

template<class T>
struct DependencyEntry<T, true> : DependencyAllocation<alignof(T)> {
    template<class... Args, class = decltype(T{std::declval<Args>()...})>
    explicit DependencyEntry(Args&&... args) : value{std::forward<Args>(args)...} {}

    T value;
};

template<class F, class = void>
struct Signature {};

template<class F>
struct Signature<F, void_t<decltype(&F::operator())>> : Signature<decltype(&F::operator())> {};

template<class R, class... Args>
struct Signature<R (*)(Args...), void> { using args = Types<Args...>; };

#define TOWER_MEMBER_SIGNATURE(QUALIFIERS)                                  \
    template<class R, class C, class... Args>                               \
    struct Signature<R (C::*)(Args...) QUALIFIERS, void> { /* NOLINT(bugprone-macro-parentheses) */ \
        using args = Types<Args...>;                                        \
    }

TOWER_MEMBER_SIGNATURE();
TOWER_MEMBER_SIGNATURE(const);
TOWER_MEMBER_SIGNATURE(volatile);
TOWER_MEMBER_SIGNATURE(const volatile);
TOWER_MEMBER_SIGNATURE(&);
TOWER_MEMBER_SIGNATURE(const &);
TOWER_MEMBER_SIGNATURE(volatile &);
TOWER_MEMBER_SIGNATURE(const volatile &);
TOWER_MEMBER_SIGNATURE(&&);
TOWER_MEMBER_SIGNATURE(const &&);
TOWER_MEMBER_SIGNATURE(volatile &&);
TOWER_MEMBER_SIGNATURE(const volatile &&);

#if defined(__cpp_noexcept_function_type)
template<class R, class... Args>
struct Signature<R (*)(Args...) noexcept, void> : Signature<R (*)(Args...)> {};

TOWER_MEMBER_SIGNATURE(noexcept);
TOWER_MEMBER_SIGNATURE(const noexcept);
TOWER_MEMBER_SIGNATURE(volatile noexcept);
TOWER_MEMBER_SIGNATURE(const volatile noexcept);
TOWER_MEMBER_SIGNATURE(& noexcept);
TOWER_MEMBER_SIGNATURE(const & noexcept);
TOWER_MEMBER_SIGNATURE(volatile & noexcept);
TOWER_MEMBER_SIGNATURE(const volatile & noexcept);
TOWER_MEMBER_SIGNATURE(&& noexcept);
TOWER_MEMBER_SIGNATURE(const && noexcept);
TOWER_MEMBER_SIGNATURE(volatile && noexcept);
TOWER_MEMBER_SIGNATURE(const volatile && noexcept);
#endif

#undef TOWER_MEMBER_SIGNATURE

template<class Container, class F, class... Args>
auto invoke_inferred(Container& container, F&& function, Types<Args...>)
    -> decltype(container.template invoke_as<Args...>(std::forward<F>(function))) {
    return container.template invoke_as<Args...>(std::forward<F>(function));
}

}  // namespace detail

inline Error make_error_code(errc value) noexcept {
    static const detail::DependencyErrorCategory category;
    return {static_cast<int>(value), category};
}

class Dependencies {
    using Entry = std::unique_ptr<void, void (*)(void*)>;

    template<class T>
    T* find() {
        auto entry = entries_.find(std::type_index(typeid(T)));
        return entry == entries_.end() ? nullptr :
            std::addressof(static_cast<detail::DependencyEntry<T>*>(entry->second.get())->value);
    }

    template<class T>
    const T* find() const {
        auto entry = entries_.find(std::type_index(typeid(T)));
        return entry == entries_.end() ? nullptr :
            std::addressof(static_cast<const detail::DependencyEntry<T>*>(entry->second.get())->value);
    }

    template<class... Args, class Container, class F, class Pointers, std::size_t... I>
    static auto invoke_resolved(Container& container, F&& function, Pointers pointers,
                                std::index_sequence<I...>)
        -> decltype(detail::call_result(std::forward<F>(function),
                                        static_cast<Args>(*std::get<I>(pointers))...)) {
        using R = decltype(detail::call_result(std::forward<F>(function),
                                               static_cast<Args>(*std::get<I>(pointers))...));
        bool found = true;
        using Expand = int[];
        // Braced initialization sequences the lookups from left to right.
        (void)Expand{0, (found = found && (std::get<I>(pointers) =
            container.template find<detail::dependency_t<Args>>()) != nullptr, 0)...};
        if (!found) {
            return failure(typename R::error_type(make_error_code(errc::missing_dependency)));
        }
        return detail::call_result(std::forward<F>(function),
                                   static_cast<Args>(*std::get<I>(pointers))...);
    }

    template<class... Args, class Container, class F,
             class Pointers = std::tuple<decltype(std::declval<Container&>()
                 .template find<detail::dependency_t<Args>>())...>,
             class R = decltype(invoke_resolved<Args...>(std::declval<Container&>(),
                 std::declval<F>(), Pointers{}, std::index_sequence_for<Args...>{})),
             std::enable_if_t<detail::all<std::integral_constant<bool,
                                  !std::is_rvalue_reference<Args>::value>...>::value &&
                              std::is_constructible<typename R::error_type, Error>::value, int> = 0>
    static R invoke_from(Container& container, F&& function) {
        return invoke_resolved<Args...>(container, std::forward<F>(function), Pointers{},
                                        std::index_sequence_for<Args...>{});
    }

public:
    Dependencies() = default;
    Dependencies(const Dependencies&) = delete;
    Dependencies& operator=(const Dependencies&) = delete;
    Dependencies(Dependencies&&) = default;
    Dependencies& operator=(Dependencies&&) = default;

    template<class T, class... Args,
             std::enable_if_t<std::is_object<T>::value &&
                              std::is_same<T, detail::dependency_t<T>>::value &&
                              (!std::is_array<T>::value || std::extent<T>::value != 0), int> = 0,
             class Stored = detail::DependencyEntry<T>,
             std::enable_if_t<std::is_constructible<Stored, Args&&...>::value, int> = 0>
    auto emplace(Args&&... args) -> Result<std::reference_wrapper<T>> {
        const auto key = std::type_index(typeid(T));
        if (entries_.find(key) != entries_.end()) {
            return failure(make_error_code(errc::duplicate_dependency));
        }
        Entry value(new Stored(std::forward<Args>(args)...), [](void* entry) {
            delete static_cast<Stored*>(entry);
        });
        auto inserted = entries_.emplace(key, std::move(value));
        if (!inserted.second) {
            return failure(make_error_code(errc::duplicate_dependency));
        }
        return success(std::ref(static_cast<Stored*>(inserted.first->second.get())->value));
    }

    template<class T, class Stored = std::decay_t<T>>
    auto insert(T&& value) -> decltype(this->template emplace<Stored>(std::forward<T>(value))) {
        return emplace<Stored>(std::forward<T>(value));
    }

    template<class T, std::enable_if_t<std::is_object<T>::value, int> = 0>
    auto get() & -> Result<std::reference_wrapper<T>> {
        auto value = find<detail::dependency_t<T>>();
        if (!value) {
            return failure(make_error_code(errc::missing_dependency));
        }
        return success(std::ref(static_cast<T&>(*value)));
    }

    template<class T, std::enable_if_t<std::is_object<T>::value, int> = 0>
    auto get() const& -> Result<std::reference_wrapper<const T>> {
        auto value = find<detail::dependency_t<T>>();
        if (!value) {
            return failure(make_error_code(errc::missing_dependency));
        }
        return success(std::cref(static_cast<const T&>(*value)));
    }

    template<class T>
    void get() const&& = delete;

    template<class... Args, class F>
    auto invoke_as(F&& function) & -> decltype(invoke_from<Args...>(*this, std::forward<F>(function))) {
        return invoke_from<Args...>(*this, std::forward<F>(function));
    }

    template<class... Args, class F>
    auto invoke_as(F&& function) const& -> decltype(invoke_from<Args...>(*this, std::forward<F>(function))) {
        return invoke_from<Args...>(*this, std::forward<F>(function));
    }

    template<class... Args, class F>
    void invoke_as(F&&) const&& = delete;

    template<class F, class Args = typename detail::Signature<std::decay_t<F>>::args>
    auto invoke(F&& function) &
        -> decltype(detail::invoke_inferred(*this, std::forward<F>(function), Args{})) {
        return detail::invoke_inferred(*this, std::forward<F>(function), Args{});
    }

    template<class F, class Args = typename detail::Signature<std::decay_t<F>>::args>
    auto invoke(F&& function) const&
        -> decltype(detail::invoke_inferred(*this, std::forward<F>(function), Args{})) {
        return detail::invoke_inferred(*this, std::forward<F>(function), Args{});
    }

    template<class F>
    void invoke(F&&) const&& = delete;

private:
    std::unordered_map<std::type_index, Entry> entries_;
};

}  // namespace tower

namespace std {

template<>
struct is_error_code_enum<tower::errc> : true_type {};

}  // namespace std
