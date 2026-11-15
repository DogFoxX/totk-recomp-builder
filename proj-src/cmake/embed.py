
import sys
from pathlib import Path

source = Path(sys.argv[1])
output = Path(sys.argv[2])

data = source.read_bytes()

values = ", ".join(f"0x{byte:02x}" for byte in data)

content = f"""#pragma once

#include <cstddef>

inline constexpr unsigned char auto_build_data[] = {{
    {values}
}};

inline constexpr std::size_t auto_build_size = sizeof(auto_build_data);
"""

output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(content, encoding="utf-8")

print(f"Embedded {source.name}: {len(data):,} bytes")
