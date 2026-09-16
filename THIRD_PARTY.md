# Third-party components

Two libraries are vendored into this repository rather than fetched at build
time, so that a clone builds and runs offline. Both are MIT-licensed, which
permits redistribution with attribution — that attribution is this file.

| Component | Version | Location | Licence |
| --- | --- | --- | --- |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.11.3 | `third_party/nlohmann/json.hpp` | MIT |
| [Chart.js](https://github.com/chartjs/Chart.js) | 4.4.1 | `server/static/vendor/chart.umd.js` | MIT |

GoogleTest is **not** vendored — CMake fetches it at configure time, so building
the test target needs network access on first run. It is also MIT-licensed.

`jugaad-data`, `pandas`, `requests`, `fastapi`, `uvicorn` and `pydantic` are
ordinary runtime dependencies installed with pip; see `pyproject.toml` and the
README's requirements section.

## Market data

The CSVs under `data/sample/` are derived from NSE's public bhavcopy archives.
They are included so the repository is runnable without a download step. NSE's
terms govern redistribution of its market data; if that is a concern for your
use, delete the sample files and regenerate them with the tools in `tools/`.
