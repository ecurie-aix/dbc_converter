#!/usr/bin/env python3
import argparse
import inspect
import re
import sys
from pathlib import Path
from typing import Tuple, Union

import cantools.database, cantools.typechecking
from jinja2 import Environment, FileSystemLoader

# Converts a DBC symbol name to a snake_case representation
# Inspired by https://stackoverflow.com/a/1176023/9156308
def pascal_to_snake(name: str) -> str:
    # Edge cases
    name = name.replace('CANopen', 'canopen')
    # Handle transitions: sequence of caps followed by a cap+lowercase (e.g. "CANopen" -> "CAN_open")
    name = re.sub(r'([A-Z]+)([A-Z][a-z])', r'\1_\2', name)
    # Handle transitions: lowercase/digit followed by uppercase (e.g. "nodeControl" -> "node_Control")
    name = re.sub(r'([a-z0-9])([A-Z])', r'\1_\2', name)

    return name.lower().replace('__', '_')

def sanitize_pascal_name(name: str) -> str:
    return name.replace('_','')

def capitalized_snake_case(name: str) -> str:
    return pascal_to_snake(name).upper()

def capitalize(name: str) -> str:
    return name.upper()

def get_message_name(message: cantools.database.Message) -> str:
    """
    Returns the name of a message in snake_case as used in the C++20 headers
    """
    return f"msg_{pascal_to_snake(message.name)}"

def get_choices_enum_name(multiplexer: cantools.database.Signal) -> str:
    """
    Returns the name of the choices enumeration for a message in snake_case as used in the C++20 headers
    """
    return f"{pascal_to_snake(multiplexer.name)}_choices"

_emitted_c_choices_enums: dict[str, dict] = {}
def should_emit_c_choices_enum(signal: cantools.database.Signal) -> bool:
    """
    Used only by the C header template. Returns True the first time a given choices enum name is seen
    (and records it), False on subsequent signals that generate the same enum name with identical
    choices. Raises if the same generated enum name is used for genuinely different choice sets
    """
    enum_name = get_choices_enum_name(signal)
    choices_signature = dict(signal.choices)
    previous = _emitted_c_choices_enums.get(enum_name)
    if previous is not None:
        if previous != choices_signature:
            raise RuntimeError(
                f"Enum name collision in C header generation: '{enum_name}' is used for signals with "
                f"different choice sets. Rename one of the conflicting signals in the DBC to disambiguate."
            )
        return False
    _emitted_c_choices_enums[enum_name] = choices_signature
    return True

def reset_c_choices_enum_tracking() -> None:
    _emitted_c_choices_enums.clear()

def sanitize_enum_name(signal: cantools.database.Signal, value, name) -> str:
    sanitized = re.sub(r'\s*[\(\[\{][^\)\]\}]*[\)\]\}]', '', str(name))  # Remove brackets and their contents
    sanitized = re.sub(r'(\s|/|-|\+)', '_', sanitized)
    sanitized = sanitized.replace('\'','').replace(':','')
    sanitized = sanitized.replace(',','_').replace('.','_')
    for svalue, sname in signal.choices.items():
        # Searches for duplicate choice names with different values
        if value != svalue and str(name) == str(sname):
            return f"{sanitized}{value}"
    if sanitized.lower() == "true" or sanitized.lower() == "false":
        return sanitized.upper() # Exception for booleans to avoid compilation issues
    return pascal_to_snake(sanitized) # Try to match the C++ standard library by using lowercase enumeration constants

def remove_common_name_sequence(name: str, message: cantools.database.Message) -> str:
    """
    Removes a sequence from the start of the name that is present in the message name and all signal names.
    """
    all_names = [message.name] + [s.name for s in message.signals]

    parts = name.split("_")
    for i in range(1, len(parts) + 1):
        prefix = "_".join(parts[:i]) + "_"
        # TODO: Is always using pascal_to_snake here okay?
        if not all(pascal_to_snake(n).startswith(prefix) for n in all_names):
            if parts[i - 1][0].isdigit():
                i = i - 1 if i > 0 else 0
            return "_".join(parts[(i - 1):])

    return name

def int_as_hex(value: int) -> str:
    return hex(value)

def get_byte_order_as_enum(order: cantools.typechecking.ByteOrder) -> str:
    """
    Returns the name of the respective byteorder enumeration used in the C++20 headers
    """
    if order == "little_endian":
        return "detail::byte_order::intel"
    elif order == "big_endian":
        return "detail::byte_order::motorola"
    else:
        raise Exception('Unknown signal byte order')

def get_int_type_by_bits(bit_len: int, signed: bool = False, prefix: str = "") -> str:
    """
    Returns the next best C++ integer type for the given bit length and signedness, with an optional prefix
    """
    if not signed:
        prefix = prefix + "u"
    if bit_len <= 8:
        return prefix + "int8_t"
    if bit_len <= 16:
        return prefix + "int16_t"
    if bit_len <= 32:
        return prefix + "int32_t"
    return prefix + "int64_t"

def get_std_int_type_by_bits(bit_len: int, signed: bool = False) -> str:
    return get_int_type_by_bits(bit_len, signed, "std::")

def get_unpacked_max_range(signal: cantools.database.Signal) -> Tuple[int, int]:
    """
    Computes the range the unpacked signal will have with signal and offset applied.
    """
    n = signal.length
    max_raw = (1 << (n - signal.is_signed)) - 1
    min_raw = -max_raw - 1 if signal.is_signed else 0

    max_phys = max_raw * signal.scale + signal.offset
    min_phys = min_raw * signal.scale + signal.offset
    return min(min_phys, max_phys), max(min_phys, max_phys)

def get_bit_len_for_range(low: int, high: int) -> int:
    if low < 0:
        return max((~low).bit_length() + 1, (high).bit_length() + 1)
    else:
        return high.bit_length()

def get_unpacked_signal_type_cpp(signal: cantools.database.Signal) -> str:
    """
    Returns the name of the appropriate type used for storing the unpacked signal value for the C++20 headers
    """
    if signal.choices and not signal_needs_conversion(signal):
        return get_choices_enum_name(signal)
    if signal.length == 1:
        return "bool"
    if type(signal.scale) is int and type(signal.offset) is int:
        min_phys, max_phys = get_unpacked_max_range(signal)
        return get_std_int_type_by_bits(get_bit_len_for_range(min_phys, max_phys), signed=(min_phys < 0))
    return "float"

def get_unpacked_signal_type_c(signal: cantools.database.Signal) -> str:
    if signal.choices and not signal_needs_conversion(signal):
        return get_choices_enum_name(signal)
    if signal.length == 1:
        return "bool"
    if type(signal.scale) is int and type(signal.offset) is int:
        min_phys, max_phys = get_unpacked_max_range(signal)
        return get_int_type_by_bits(get_bit_len_for_range(min_phys, max_phys), signed=(min_phys < 0))
    return "float"

def get_signal_range(signal: cantools.database.Signal) -> str:
    if signal.maximum is None or signal.minimum is None:
        return ""
    return f" ({signal.minimum}, {signal.maximum})"

def signal_needs_conversion(signal: cantools.database.Signal) -> bool:
    if type(signal.scale) is int and type(signal.offset) is int and signal.scale == 1 and signal.offset == 0:
        return False
    return True

def get_conversion_data_suffix(value: Union[int, float]) -> str:
    stringified = str(value)
    if 'e' in stringified:
        return "f"
    if type(value) is int or '.' not in stringified:
        return ""
    return "f"

def get_multiplexed_signal_lists(message: cantools.database.Message) -> list[tuple[int, list[cantools.database.Signal]]]:
    """
    Returns a flattened list of a tuple consisting of each value that the multiplexer signal can be and the
    corresponding list of signals that are active for that value. Note that this only works with simple multiplexing,
    so only when a single signal acts as a multiplexer.
    """
    assert(message.is_multiplexed())

    multiplexers: dict[cantools.database.Signal, dict[int, list[cantools.database.Signal]]] = {}
    for signal in message.signals:
        if signal.multiplexer_signal is not None:
            # This signal is multiplexed by some multiplexer signal
            mux = message.get_signal_by_name(signal.multiplexer_signal)
            mux_id = signal.multiplexer_ids[0] if signal.multiplexer_ids else None
            if mux not in multiplexers:
                multiplexers[mux] = dict()
            if mux_id is not None:
                if mux_id not in multiplexers[mux]:
                    multiplexers[mux][mux_id] = list()
                multiplexers[mux][mux_id].append(signal)

    assert(len(multiplexers) == 1) # This function would need to generate every possible combinations, which it just doesn't
    multiplexer, combinations = multiplexers.popitem()
    for mux_id, signals in combinations.items():
        signals.append(multiplexer)
        signals.sort(key=lambda x: x.start)
    return list(combinations.items())

def get_multiplexer(message: cantools.database.Message) -> cantools.database.Signal:
    multiplexer_list = [signal for signal in message.signals if signal.is_multiplexer]
    assert(len(multiplexer_list) <= 1)
    if not multiplexer_list:
        raise RuntimeError('Tried getting multiplexer signal on message that isn\'t multiplexed')
    assert(multiplexer_list[0].choices != None) # We require choices to be defined for multiplexer generation
    return multiplexer_list[0]

def get_signal_list(database: cantools.database.Database) -> list[cantools.database.Signal]:
    return list({signal.name: signal for msg in database.messages for signal in msg.signals}.values())

def generate_c_header(env: Environment) -> Tuple[str, Path]:
    dbc_name = env.globals['dbc_name']
    print(f"Generating C header for {dbc_name}")
    header_name = f"{dbc_name.upper()}_H"

    template = env.get_template('c_template.h')
    return template.render(header_name=header_name), Path(f"c/{dbc_name}.h")

def generate_cpp20_header(env: Environment) -> Tuple[str, Path]:
    dbc_name = env.globals['dbc_name']
    print(f"Generating C++20 header for {dbc_name}")
    header_name = f"{dbc_name.upper()}_HPP"

    template = env.get_template('cpp20_template.hpp')
    return template.render(header_name=header_name), Path(f"cpp20/{dbc_name}.hpp")

def open_output_file(output_dir: Path, output_file: Path, mode):
    target_file = Path(output_dir) / output_file
    target_file.parent.mkdir(exist_ok=True)
    return open(target_file, mode)

def write_generated_file(output_dir: Path, output_file: Path, generated: str):
    with open_output_file(output_dir, output_file, 'wb') as f_out:
        f_out.write(generated.encode('utf-8')) # DBCs use cp1252, we want UTF-8 for generated files

def validate_generated_file(output_dir: Path, output_file: Path, generated: str):
    try:
        with open_output_file(output_dir, output_file, 'rb') as f:
            current = f.read()
        if current != generated.encode('utf-8'):
            print(f"{output_file} is out of date. Run the generator script")
            sys.exit(1)
    except FileNotFoundError:
        print(f"{output_file} doesn't exist. Run the generator script")
        sys.exit(1)

def generate(dbc_dir: str, output_dir: str, targets: list[str], validate: bool):
    dbc_dir = Path(dbc_dir)
    if not dbc_dir.exists():
        raise ValueError("The given DBC directory does not exist")

    output_dir = Path(output_dir)
    output_dir.mkdir(exist_ok=True)

    dbc_conv_folder = Path(__file__).parent
    env_loader = FileSystemLoader(searchpath=dbc_conv_folder / 'templates')
    env = Environment(
        loader=env_loader,
        trim_blocks=True,
        lstrip_blocks=True
    )

    # All the custom functions used in the templates for manipulating strings, stringifying objects, ...
    # This module stuff got cooked by ChatGPT idk if this is the intended solution
    current_module = sys.modules[__name__]
    for name, obj in inspect.getmembers(current_module, inspect.isfunction):
        env.globals[name] = obj

    header_generators = {
        'c': generate_c_header,
        'cpp20': generate_cpp20_header,
    }

    dbc_files = Path(dbc_dir).glob('*.dbc')
    for dbc in dbc_files:
        dbc_name = dbc.stem
        db = cantools.database.load_file(dbc)
        reset_c_choices_enum_tracking()

        for message in db.messages:
            if not message.is_multiplexed():
                continue
            multiplexer_signals = [signal for signal in message.signals if signal.is_multiplexer]
            if len(multiplexer_signals) > 1:
                raise RuntimeError('Can\'t generate headers for DBC with \'Extended Signal Multiplexing\'')

        signal_list = get_signal_list(db)
        env.globals.update(
            database=db,
            signals=signal_list,
            dbc_name=dbc_name,
        )

        for name, generator in header_generators.items():
            if name not in targets:
                continue
            generated, output_file = generator(env)
            if validate:
                validate_generated_file(output_dir, output_file, generated)
            else:
                write_generated_file(output_dir, output_file, generated)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(prog=Path(__file__).name)
    parser.add_argument('--output_dir', default='output')
    parser.add_argument('--dbc_dir', default='dbc')
    parser.add_argument('--targets', nargs='+', default=['c', 'cpp20'])
    parser.add_argument('--validate', action="store_true")
    args = parser.parse_args()

    generate(args.dbc_dir, args.output_dir, args.targets, args.validate)
