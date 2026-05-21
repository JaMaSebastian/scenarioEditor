D:
cd "\_work2\c++\unreal_5_7_4\ScenarioEditor"

rmdir /s /q build
mkdir build
cd build

cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release