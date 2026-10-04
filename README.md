# DBC Converter

Python tools for generating headers and other files from Vector DBC files. This uses [cantools](https://github.com/cantools/cantools) for parsing DBC files and [Jinja2](https://jinja.palletsprojects.com/) for rendering templates.

The current setup can be easily extended for more headers for different languages or other special uses, simply by adding a new template into the `templates` folder.

## Usage

```sh
python3 -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
```

To convert all DBCs in a directory and write the generated files to an output directory:
```sh
./dbc_conv.py --dbc_dir dbc --output_dir output
```

By default, every target is generated. To pick specific ones, pass a space-separated list to `--targets`:
```sh
./dbc_conv.py --targets cpp20
./dbc_conv.py --targets c cpp20
```

Currently available targets are:
- `c`: C99 headers, written to `output/c/<name>.h`
- `cpp20`: C++20 headers, written to `output/cpp20/<name>.hpp`

## Example

Say you have a `vehicle.dbc` with this message in it:

```
BO_ 512 InverterStatus: 2 INV
 SG_ State : 0|3@1+ (1,0) [0|7] "" VCU
 SG_ Temperature : 8|8@1+ (1,-40) [-40|215] "degC" VCU

BA_ "GenMsgCycleTime" BO_ 512 100;
VAL_ 512 State 0 "Off" 1 "Precharge" 2 "Ready" 3 "Error" ;
```

The generated `vehicle.hpp` puts everything in a `vehicle` namespace and turns each message into a struct. Names become snake_case, signals get the smallest type that fits their scaled range, and value tables become enums:

```cpp
namespace vehicle {
struct msg_inverter_status {
	static constexpr std::uint16_t frame_id = 0x200;
	static constexpr std::size_t frame_len = 2;
	static constexpr std::uint32_t cycle_time = 100;
	static constexpr auto cycle_duration = std::chrono::milliseconds(cycle_time);

	enum class state_choices : std::uint8_t { off = 0, precharge = 1, ready = 2, error = 3 };

	state_choices state; ///< State (0, 7)
	std::int16_t temperature; ///< Temperature - degC (-40, 215)

	static auto from_bytes(std::span<const std::uint8_t, frame_len> bytes);
	void write_to(std::span<std::uint8_t, frame_len> bytes) const noexcept;
	auto as_bytes() const noexcept; // returns std::array<std::uint8_t, frame_len>
};
}
```

Reading and writing a frame then looks like this:

```cpp
#include <cstdio>
#include "vehicle.hpp"

using vehicle::msg_inverter_status;

void on_can_rx(std::uint32_t id, std::span<const std::uint8_t> data) {
	if (id != msg_inverter_status::frame_id) {
		return;
	}

	// Checks the length and throws if the frame is too short
	auto status = vehicle::read_msg_from_range<msg_inverter_status>(data);
	if (status.state == msg_inverter_status::state_choices::ready) {
		std::printf("Inverter ready at %d degC\n", status.temperature);
	}
}

void send_status() {
	msg_inverter_status status {
		.state = msg_inverter_status::state_choices::error,
		.temperature = 85,
	};
	can_send(status.frame_id, status.as_bytes());
}
```

## Limitations

- Only simple multiplexing (one multiplexer signal per message). DBCs with extended multiplexing are rejected.
- Multiplexer signals need a value table, since that's what the generated variants are named after.

## License

MIT, see [LICENSE.md](LICENSE.md).
