#pragma once

namespace lib {
    class UniqueFd {
      public:
        explicit UniqueFd(int descriptor = -1) noexcept;
        ~UniqueFd();

        UniqueFd(const UniqueFd &) = delete;

        auto operator=(const UniqueFd &) -> UniqueFd & = delete;

        UniqueFd(UniqueFd &&other) noexcept;

        auto operator=(UniqueFd &&other) noexcept -> UniqueFd &;
        [[nodiscard]] auto get() const noexcept -> int;
        [[nodiscard]] auto release() noexcept -> int;
        void reset() noexcept;

      private:
        int descriptor_;
    };
} // namespace lib
