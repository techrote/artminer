#pragma once

#include <cassert>
#include <optional>
#include <utility>
#include <variant>

namespace artminer::core {

template <typename T, typename E>
class Result {
public:
    [[nodiscard]] static Result success(T value) {
        return Result(Storage{std::in_place_index<0>, std::move(value)});
    }

    [[nodiscard]] static Result failure(E error) {
        return Result(Storage{std::in_place_index<1>, std::move(error)});
    }

    [[nodiscard]] bool is_ok() const noexcept {
        return storage_.index() == 0;
    }

    [[nodiscard]] bool is_error() const noexcept {
        return !is_ok();
    }

    [[nodiscard]] T& value() & {
        assert(is_ok());
        return std::get<0>(storage_);
    }

    [[nodiscard]] const T& value() const& {
        assert(is_ok());
        return std::get<0>(storage_);
    }

    [[nodiscard]] T&& value() && {
        assert(is_ok());
        return std::get<0>(std::move(storage_));
    }

    [[nodiscard]] E& error() & {
        assert(is_error());
        return std::get<1>(storage_);
    }

    [[nodiscard]] const E& error() const& {
        assert(is_error());
        return std::get<1>(storage_);
    }

private:
    using Storage = std::variant<T, E>;

    explicit Result(Storage storage) : storage_(std::move(storage)) {}

    Storage storage_;
};

template <typename E>
class Result<void, E> {
public:
    [[nodiscard]] static Result success() {
        return Result(std::nullopt);
    }

    [[nodiscard]] static Result failure(E error) {
        return Result(std::optional<E>{std::move(error)});
    }

    [[nodiscard]] bool is_ok() const noexcept {
        return !error_.has_value();
    }

    [[nodiscard]] bool is_error() const noexcept {
        return error_.has_value();
    }

    [[nodiscard]] E& error() & {
        assert(is_error());
        return *error_;
    }

    [[nodiscard]] const E& error() const& {
        assert(is_error());
        return *error_;
    }

private:
    explicit Result(std::optional<E> error) : error_(std::move(error)) {}

    std::optional<E> error_;
};

}  // namespace artminer::core
