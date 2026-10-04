#ifndef {{ header_name }}
#define {{ header_name }}

#include <bit>
#include <chrono>
#include <cstdint>
#include <limits>
#include <span>
#include <ranges>
#include <vector>

#ifdef __cpp_exceptions
#include <stdexcept>
#endif

#if __cplusplus >= 202302L
#include <utility>
#define {{ header_name }}_UNREACHABLE std::unreachable();
#elif defined(__GNUC__) || defined(__clang__)
#define {{ header_name }}_UNREACHABLE __builtin_unreachable();
#elif defined(_MSC_VER)
#define {{ header_name }}_UNREACHABLE __assume(false);
#else
#define {{ header_name }}_UNREACHABLE assert(0);
#endif

namespace {{ dbc_name }} {
namespace detail {
enum class byte_order {
	intel,
	motorola
};

[[nodiscard]] constexpr std::uint8_t bitmask_for(const std::size_t n, const std::size_t offset = 0) {
	return static_cast<std::uint8_t>(((1 << n) - 1) << offset);
}

[[nodiscard]] constexpr auto reverse_byte(std::uint8_t b) {
	b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
	b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
	b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
	return b;
}

template <std::integral T, std::size_t bit_len>
struct bit_range_t {
	T min;
	T max;
};

template <std::integral T, std::size_t bit_len>
[[nodiscard]] consteval bit_range_t<T, bit_len> bit_range() noexcept {
	static_assert(bit_len > 0, "bit_len must be positive");
	static_assert(bit_len <= sizeof(T) * 8, "bit_len exceeds width of T");

	if constexpr (std::is_signed_v<T>) {
		if constexpr (bit_len == sizeof(T) * 8) {
			return {std::numeric_limits<T>::min(), std::numeric_limits<T>::max()};
		} else {
			return {static_cast<T>(-(T{1} << (bit_len - 1))),
					static_cast<T>((T{1} << (bit_len - 1)) - 1)};
		}
	} else {
		if constexpr (bit_len == sizeof(T) * 8) {
			return {T{0}, std::numeric_limits<T>::max()};
		} else {
			return {T{0}, static_cast<T>((T{1} << bit_len) - 1)};
		}
	}
}

[[nodiscard]] constexpr auto conv_bit_idx(std::size_t bit_idx) noexcept {
	return bit_idx - (bit_idx % 8) + 7 - (bit_idx % 8);
}

template <std::integral T, byte_order E, std::size_t bit_len, std::size_t bit_off, std::size_t N>
[[nodiscard, gnu::always_inline]] constexpr auto deserialize_bits(std::span<const std::uint8_t, N> bytes) requires (!std::is_same_v<T, bool>) {
	T ret = 0;
	if constexpr (E == byte_order::motorola) {
		for (std::size_t i = 0, s = conv_bit_idx(bit_off); i < bit_len; ++i, ++s) {
			auto byte = reverse_byte(bytes[s >> 3]);
			ret |= static_cast<T>((byte >> (s & 7)) & 1) << (bit_len - i - 1);
		}
	} else {
		for (std::size_t i = 0, s = bit_off; i < bit_len; ++i, ++s) {
			const auto& byte = bytes[s >> 3];
			ret |= static_cast<T>((byte >> (s & 7)) & 1) << i;
		}
	}
	return ret;
}

template <std::integral T, byte_order E, std::size_t bit_len, std::size_t bit_off, typename U, std::size_t N>
[[gnu::always_inline]] constexpr void serialize_bits(U value, std::span<std::uint8_t, N> bytes) requires (!std::is_same_v<T, bool>) {
	constexpr auto range = bit_range<T, bit_len>();
	const auto converted = [&]() {
		if constexpr (std::is_enum_v<U>) {
			// TODO: Have some compile-time range check to see if enums fit into the signal properly?
			return static_cast<T>(value);
		} else {
			return static_cast<T>(std::clamp<std::common_type_t<U, T>>(value, range.min, range.max));
		}
	}();
	const auto input = std::as_bytes(std::span(&converted, 1));
	if constexpr (E == byte_order::motorola) {
		for (std::size_t i = 0, s = conv_bit_idx(bit_off); i < bit_len; ++i, ++s) {
			auto byte = reverse_byte(bytes[s >> 3]);
			byte |= (static_cast<std::uint8_t>(converted >> (bit_len - i - 1)) & 1) << (s & 7);
			bytes[s >> 3] = reverse_byte(byte);
		}
	} else {
		for (std::size_t i = 0; i < bit_len; ++i) {
			const auto bit_idx = i % 8;
			auto& byte = bytes[(bit_off + i) >> 3];
			byte |= ((static_cast<std::uint8_t>(input[i >> 3]) & bitmask_for(1, bit_idx)) >> bit_idx) << ((bit_off + i) % 8);
		}
	}
}

template <typename T, typename U, typename V>
[[nodiscard, gnu::always_inline]] constexpr auto pack_value(T value, U scale, V offset) {
	return (value - offset) / scale;
}
template <typename T, typename U, typename V>
[[nodiscard, gnu::always_inline]] constexpr auto unpack_value(T value, U scale, V offset) {
	return value * scale + offset;
}
template <typename R, typename T, typename U, typename V>
[[nodiscard, gnu::always_inline]] constexpr auto unpack_value(T value, U scale, V offset) {
	return static_cast<R>(value * scale + offset);
}
}

{% macro deserialize_member(message, signal, indent='', name_prefix='') %}
{% if signal_needs_conversion(signal) or signal.choices %}
{{indent}}msg.{{name_prefix}}{{ remove_common_name_sequence(pascal_to_snake(signal.name), message) }} = detail::unpack_value{% if signal.choices %}<{{ remove_common_name_sequence(get_choices_enum_name(signal), message) }}>{% endif %}(
{{indent}}	detail::deserialize_bits<{{ get_std_int_type_by_bits(signal.length, signal.is_signed) }}, {{ get_byte_order_as_enum(signal.byte_order) }}, {{ signal.length }}, {{ signal.start }}>(bytes),
{{indent}}	{{ signal.scale }}{{ get_conversion_data_suffix(signal.scale) }}, {{ signal.offset }}{{ get_conversion_data_suffix(signal.offset) }});
{% else %}
{{indent}}msg.{{name_prefix}}{{ remove_common_name_sequence(pascal_to_snake(signal.name), message) }} = detail::deserialize_bits<{{ get_std_int_type_by_bits(signal.length, signal.is_signed) }}, {{ get_byte_order_as_enum(signal.byte_order) }}, {{ signal.length }}, {{ signal.start }}>(bytes);
{% endif %}
{% endmacro %}
{# #}
{% macro serialize_member(message, signal, indent='', name_prefix='') %}
{% if signal_needs_conversion(signal) %}
{{indent}}detail::serialize_bits<{{ get_std_int_type_by_bits(signal.length, signal.is_signed) }}, {{ get_byte_order_as_enum(signal.byte_order) }}, {{ signal.length }}, {{ signal.start }}>(
{{indent}}	detail::pack_value({{ name_prefix }}{{ remove_common_name_sequence(pascal_to_snake(signal.name), message) }}, {{ signal.scale }}{{ get_conversion_data_suffix(signal.scale) }}, {{ signal.offset }}{{ get_conversion_data_suffix(signal.offset) }}), bytes);
{% else %}
{{indent}}detail::serialize_bits<{{ get_std_int_type_by_bits(signal.length, signal.is_signed) }}, {{ get_byte_order_as_enum(signal.byte_order) }}, {{ signal.length }}, {{ signal.start }}>({{ name_prefix }}{{ remove_common_name_sequence(pascal_to_snake(signal.name), message) }}, bytes);
{% endif %}
{% endmacro %}
{# #}
{% macro define_signal(message, signal, indent='') %}
{{indent}}{{ remove_common_name_sequence(get_unpacked_signal_type_cpp(signal), message) }} {{ remove_common_name_sequence(pascal_to_snake(signal.name), message) }}; ///< {{ signal.name }}{% if not signal.unit is none %} - {{ signal.unit }}{% endif %}{{ get_signal_range(signal) }}
{% endmacro %}
{# #}
{% for message in database.messages +%}
/* {{ message.name }} ({{ int_as_hex(message.frame_id) }}) */
struct {{ get_message_name(message) }} {
	{# TODO: Support extended identifiers? #}
	static constexpr std::uint16_t frame_id = {{ int_as_hex(message.frame_id) }};
	static constexpr std::size_t frame_len = {{ message.length }};
{% if not message.cycle_time is none %}
	static constexpr std::uint32_t cycle_time = {{ message.cycle_time }};
{% else %}
	static constexpr std::uint32_t cycle_time = 0;
{% endif %}
	static constexpr auto cycle_duration = std::chrono::milliseconds(cycle_time);

	{% for signal in message.signals %}
	{% if signal.choices %}
	enum class {{ remove_common_name_sequence(get_choices_enum_name(signal), message) }} : {{ get_std_int_type_by_bits(signal.length, signal.is_signed) }} {
		{% for value, name in signal.choices | dictsort %}
		{% if value >= 2 ** signal.length or signal.is_signed and value >= 2 ** (signal.length - 1) %}
		{# Thanks for the DBC for sometimes assuming twos complementary and representing values in binary #}
		{{ sanitize_enum_name(signal, value, name) }} = static_cast<{{ get_std_int_type_by_bits(signal.length, signal.is_signed) }}>({{ value }}),
		{% else %}
		{{ sanitize_enum_name(signal, value, name) }} = {{ value }},
		{% endif %}
		{% endfor %}
	};
	{% endif %}
	{% endfor %}

	{% if message.is_multiplexed() %}
	{% set multiplexer = get_multiplexer(message) %}
	{{ define_signal(message, multiplexer) }}
	{% for value, name in multiplexer.choices | dictsort %}
	struct Mux{{ value }} {
		{% for signal in message.signals %}
		{% if signal.multiplexer_ids and value in signal.multiplexer_ids -%}
		{{ define_signal(message, signal, '\t\t') }}
		{%- endif %}
		{% endfor %}
		auto operator<=>(const Mux{{ value }}& other) const = default;
	};
	{% endfor %}
	union {
	{% for value, name in multiplexer.choices | dictsort %}
		Mux{{ value }} mux{{ value }};
	{% endfor %}
	};
	{% else %}
	{% for signal in message.signals -%}
	{{ define_signal(message, signal, '\t') }}
	{%- endfor %}
	{% endif %}

	{% if message.signals %}
	[[nodiscard]] static auto from_bytes(const std::span<const std::uint8_t, frame_len> bytes) {
		{{ get_message_name(message) }} msg {};
		{% if message.is_multiplexed() %}
		{% set multiplexer = get_multiplexer(message) %}
{{ deserialize_member(message, multiplexer, '\t\t') }}
		switch (msg.{{ remove_common_name_sequence(pascal_to_snake(multiplexer.name), message) }}) {
			using enum {{ remove_common_name_sequence(get_choices_enum_name(multiplexer), message) }};
			{% for value, name in multiplexer.choices | dictsort %}
			case {{ sanitize_enum_name(multiplexer, value, name) }}: {
				{% for signal in message.signals -%}
				{% if signal.multiplexer_ids and value in signal.multiplexer_ids -%}
{{ deserialize_member(message, signal, '\t\t\t\t', 'mux' ~ value ~ '.') }}
				{%- endif %}
				{%- endfor %}
				break;
			}
			{% endfor %}
		}
		{% else %}
		{% for signal in message.signals -%}
{{ deserialize_member(message, signal, '\t\t') }}
		{%- endfor %}
		{% endif %}
		return msg;
	}
	void write_to(const std::span<uint8_t, frame_len> bytes) const noexcept {
		{% if message.is_multiplexed() %}
		{% set multiplexer = get_multiplexer(message) %}
{{ serialize_member(message, multiplexer, '\t\t') }}
		switch ({{ remove_common_name_sequence(pascal_to_snake(multiplexer.name), message) }}) {
			using enum {{ remove_common_name_sequence(get_choices_enum_name(multiplexer), message) }};
			{% for value, name in multiplexer.choices | dictsort %}
			case {{ sanitize_enum_name(multiplexer, value, name) }}: {
				{% for signal in message.signals -%}
				{% if signal.multiplexer_ids and value in signal.multiplexer_ids -%}
{{ serialize_member(message, signal, '\t\t\t\t', 'mux' ~ value ~ '.') }}
				{%- endif %}
				{%- endfor %}
				break;
			}
			{% endfor %}
		}
		{% else %}
		{% for signal in message.signals -%}
{{ serialize_member(message, signal, '\t\t') }}
		{%- endfor %}
		{% endif %}
	}
	[[nodiscard]] auto as_bytes() const noexcept {
		std::array<std::uint8_t, frame_len> bytes {};
		write_to(bytes);
		return bytes;
	}
	{% endif %}

	{% if message.is_multiplexed() %}
	{% set multiplexer = get_multiplexer(message) %}
	std::partial_ordering operator<=>(const {{ get_message_name(message) }}& other) const {
		if (const auto cmp = {{ remove_common_name_sequence(pascal_to_snake(multiplexer.name), message) }} <=> other.{{ remove_common_name_sequence(pascal_to_snake(multiplexer.name), message) }};
			cmp != std::strong_ordering::equivalent)
			return cmp;
		switch ({{ remove_common_name_sequence(pascal_to_snake(multiplexer.name), message) }}) {
			using enum {{ remove_common_name_sequence(get_choices_enum_name(multiplexer), message) }};
			{% for value, name in multiplexer.choices | dictsort %}
			case {{ sanitize_enum_name(multiplexer, value, name) }}: return mux{{ value }} <=> other.mux{{ value }};
			{% endfor %}
			default: {{ header_name }}_UNREACHABLE
		}
	}
	auto operator==(const {{ get_message_name(message) }}& other) const {
		return (*this <=> other) == std::partial_ordering::equivalent;
	}
	{% else %}
	auto operator<=>(const {{ get_message_name(message) }}&) const = default;
	{% endif %}
};
{% endfor %}

template <typename Msg, typename R>
requires std::ranges::range<R> && std::ranges::contiguous_range<R> && std::same_as<std::ranges::range_value_t<R>, std::uint8_t>
[[nodiscard]] Msg read_msg_from_range(const R& range) {
	constexpr auto len = Msg::frame_len;
#ifdef __cpp_exceptions
	if (std::ranges::size(range) < len)
		throw std::runtime_error("Tried reading CAN message from range with insufficient bytes");
#endif
	return Msg::from_bytes(std::span(range).template first<len>());
}

[[nodiscard]] std::vector<std::uint8_t> write_msg_to_vector(const auto& message) {
	constexpr auto len = message.frame_len;
	std::vector<std::uint8_t> vector(len);
	message.write_to(std::span(vector).template first<len>());
	return vector;
}
}
#endif
