#ifndef {{ header_name }}
#define {{ header_name }}

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static inline uint8_t _dbc_bitmask_for(size_t n, size_t offset) {
	return (((1u << n) - 1) << offset) & 0xFF;
}

static inline uint8_t _dbc_reverse_byte(uint8_t b) {
	b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
	b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
	b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
	return b;
}

static inline size_t _dbc_conv_bit_idx(size_t bit_idx) {
	return bit_idx - (bit_idx % 8) + 7 - (bit_idx % 8);
}

static inline uint64_t _deserialize_bits_intel(const uint8_t* bytes, uint8_t start, uint8_t len) {
	uint64_t ret = 0;
	for (size_t i = 0; i < len; i++) {
		size_t s = start + i;
		if (bytes[s >> 3] >> (s & 7) & 1) {
			ret |= (uint64_t)1u << i;
		}
	}
	return ret;
}

static inline uint64_t _deserialize_bits_motorola(const uint8_t* bytes, uint8_t start, uint8_t len) {
	uint64_t ret = 0;
	size_t s = _dbc_conv_bit_idx(start);
	for (size_t i = 0; i < len; i++, s++) {
		uint8_t b = _dbc_reverse_byte(bytes[s >> 3]);
		if ((b >> (s & 7)) & 1) {
			ret |= (uint64_t)1u << (len - i - 1);
		}
	}
	return ret;
}

static inline void _serialize_bits_intel(uint64_t val, uint8_t* bytes, uint8_t start, uint8_t len) {
	for (size_t i = 0; i < len; i++) {
		size_t s = start + i;
		if ((val >> i) & 1) {
			bytes[s >> 3] |= (uint8_t)(1u << (s & 7));
		} else {
			bytes[s >> 3] &= ~((uint8_t)(1u << (s & 7)));
		}
	}
}

static inline void _serialize_bits_motorola(uint64_t val, uint8_t* bytes, uint8_t start, uint8_t len) {
	size_t s = _dbc_conv_bit_idx(start);
	for (uint8_t i = 0; i < len; i++, s++) {
		uint8_t b = _dbc_reverse_byte(bytes[s >> 3]);
		if ((val >> (len - 1u - i)) & 1u)
			b |= ((uint8_t)(1u << (s & 7)));
		else
			b &= ~((uint8_t)(1u << (s & 7)));
		bytes[s >> 3] = _dbc_reverse_byte(b);
	}
}

static inline int64_t _sign_extend(uint64_t val, uint8_t len) {
	uint64_t sign_bit = (uint64_t)1u << (len - 1);
	return (int64_t)((val ^ sign_bit) - sign_bit);
}

static inline uint64_t _pack_value(float value, float scale, float offset) {
	return (uint64_t)(int64_t)((value - offset) / scale);
}

static inline float _unpack_value(int64_t value, float scale, float offset) {
	return (float)(value * scale + offset);
}

{%- macro define_signal(signal, message, indent='') %}
{{indent}}{% if signal.choices and not signal_needs_conversion(signal) %}{{ get_choices_enum_name(signal) }}{% else %}{{ get_unpacked_signal_type_c(signal) }}{% endif %} {{ remove_common_name_sequence(pascal_to_snake(signal.name), message) }}; ///< {{ signal.name }}{% if not signal.unit is none %} - {{ signal.unit }}{% endif %}{{ get_signal_range(signal) }}
{% endmacro %}
{% macro deserialize_member(signal, dest) %}
{% set fn = '_deserialize_bits_motorola' if signal.byte_order == 'big_endian' else '_deserialize_bits_intel' %}
{% if signal.is_signed %}
{% set raw %}_sign_extend({{ fn }}(bytes, {{ signal.start }}u, {{ signal.length }}u), {{ signal.length }}u){% endset %}
{% else %}
{% set raw %}{{ fn }}(bytes, {{ signal.start }}u, {{ signal.length }}u){% endset %}
{% endif %}
{% if signal_needs_conversion(signal) %}
	{{ dest }} = _unpack_value({{ raw }}, {{ signal.scale }}{{ get_conversion_data_suffix(signal.scale) }}, {{ signal.offset }}{{ get_conversion_data_suffix(signal.offset) }});
{% else %}
	{{ dest }} = {{ raw }};
{% endif %}
{% endmacro %}
{% macro serialize_member(signal, src, indent='') %}
{% set fn = '_serialize_bits_motorola' if signal.byte_order == 'big_endian' else '_serialize_bits_intel' %}
{% if signal_needs_conversion(signal) %}
{{ indent }}{{ fn }}(_pack_value({{ src }}, {{ signal.scale }}{{ get_conversion_data_suffix(signal.scale) }}, {{ signal.offset }}{{ get_conversion_data_suffix(signal.offset) }}), bytes, {{ signal.start }}u, {{ signal.length }}u);
{% else %}
{{ indent }}{{ fn }}((uint64_t)({{ src }}), bytes, {{ signal.start }}u, {{ signal.length }}u);
{% endif %}
{% endmacro %}

{% for message in database.messages +%}
/* {{message.name}}  ({{ int_as_hex(message.frame_id) }})*/
#define CANID_MSG_{{ capitalize(message.name) }} {{ int_as_hex(message.frame_id) }}u
#define LENGTH_MSG_{{ capitalize(message.name) }} {{ message.length }}u
{% if message.cycle_time %}
#define CYCLE_MSG_{{ capitalize(message.name) }} {{ message.cycle_time }}u
{% else %}
#define CYCLE_MSG_{{ capitalize(message.name) }} 0u
{% endif %}

{% for signal in message.signals if signal.choices and should_emit_c_choices_enum(signal) %}
typedef enum {
	{% for value, name in signal.choices | dictsort %}
	{{ pascal_to_snake(signal.name) }}_{{ sanitize_enum_name(signal, value, name) }} = {{value}},
	{% endfor %}
} {{ get_choices_enum_name(signal) }};
{% endfor %}


{% if message.is_multiplexed() %}
{% set multiplexer = get_multiplexer(message) %}
{% for mux_id, signals in get_multiplexed_signal_lists(message) %}
typedef struct {
	{% for signal in signals if signal.name != multiplexer.name %}
	{{ define_signal(signal, message) }}
	{%- endfor %}
} msg_{{ pascal_to_snake(message.name)}}_mux{{mux_id}};
{% endfor %}

typedef struct {
	{{ define_signal(multiplexer, message) }}
	union {
{% for mux_id, _ in get_multiplexed_signal_lists(message) %}
		msg_{{ pascal_to_snake(message.name) }}_mux{{ mux_id }} mux{{ mux_id }};
{% endfor %}
	} mux;
} msg_{{ pascal_to_snake(message.name) }};
{% else %}
typedef struct {
{% for signal in message.signals -%}
	{{ define_signal(signal, message, '\t') }}
{%- endfor %}
{% if not message.signals %}
	uint8_t _unused;
{% endif %}
} msg_{{ pascal_to_snake(message.name) }};
{% endif %}

static inline void {{ pascal_to_snake(message.name) }}_from_bytes( const uint8_t* bytes, msg_{{ pascal_to_snake(message.name) }}* msg) {
	{% if not message.signals %}
	(void) bytes;
	(void) msg;
	{% endif %}
	{% if message.is_multiplexed()%}
	{% set multiplexer = get_multiplexer(message) %}
{{ deserialize_member(multiplexer, 'msg->' ~ remove_common_name_sequence(pascal_to_snake(multiplexer.name), message)) }}
	switch (msg->{{ remove_common_name_sequence(pascal_to_snake(multiplexer.name), message) }}) {
		{% for value, name in multiplexer.choices | dictsort %}
		case {{ pascal_to_snake(multiplexer.name) }}_{{ sanitize_enum_name(multiplexer, value, name) }}:
		{% for signal in message.signals -%}
		{% if signal.multiplexer_ids and value in signal.multiplexer_ids -%}
		{{ deserialize_member(signal, '\t\t' ~ 'msg->mux.mux' ~ value ~ '.' ~ remove_common_name_sequence(pascal_to_snake(signal.name), message)) }}
		{%- endif %}
		{% endfor %}
			break;
		{% endfor %}
			default:
				break;
	}
	{% else %}
	{% for signal in message.signals %}
{{ deserialize_member(signal, 'msg->' ~ remove_common_name_sequence(pascal_to_snake(signal.name), message)) }}
	{%- endfor %}
	{% endif %}
}

static inline void {{ pascal_to_snake(message.name) }}_write_to(uint8_t *bytes, const msg_{{ pascal_to_snake(message.name) }} *msg) {
	memset(bytes, 0, LENGTH_MSG_{{ capitalize(message.name) }});
{% if not message.signals %}
	(void) bytes;
	(void) msg;
{% endif %}
{% if message.is_multiplexed() %}
{% set multiplexer = get_multiplexer(message) %}
{{ serialize_member(multiplexer, 'msg->' ~ remove_common_name_sequence(pascal_to_snake(multiplexer.name), message), '\t') }}
	switch (msg->{{ remove_common_name_sequence(pascal_to_snake(multiplexer.name), message) }}) {
{% for value, name in multiplexer.choices | dictsort %}
		case {{ pascal_to_snake(multiplexer.name) }}_{{ sanitize_enum_name(multiplexer, value, name) }}:
{% for signal in message.signals -%}
{% if signal.multiplexer_ids and value in signal.multiplexer_ids -%}
		{{ serialize_member(signal, 'msg->mux.mux' ~ value ~ '.' ~ remove_common_name_sequence(pascal_to_snake(signal.name), message), '\t\t\t') }}
{%- endif %}
{%- endfor %}
			break;
{% endfor %}
		default: 
			break;
	}
{% else %}
{% for signal in message.signals %}
{{ serialize_member(signal, 'msg->' ~ remove_common_name_sequence(pascal_to_snake(signal.name), message), '\t') }}
{%- endfor %}
{% endif %}
}

{% endfor %}

#endif
