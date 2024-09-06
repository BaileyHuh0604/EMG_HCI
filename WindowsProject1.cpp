﻿#include <iostream>
#include <iomanip>
#include <sstream>
#include <thread>
#include <vector>
#include <deque>
#include <chrono>
#include <mutex>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <windows.h>

using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Devices::Bluetooth;
using namespace Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace Windows::Storage::Streams;
using namespace Windows::Devices::Bluetooth::Advertisement;

bool running = false;
std::deque<std::vector<int>> emgdata;
const GUID characteristic_uuid = { 0xbeb5483e, 0x36e1, 0x4688, { 0xb7, 0xf5, 0xea, 0x07, 0x36, 0x1b, 0x26, 0xa8 } };
std::mutex device_mutex;
BluetoothLEDevice global_device = nullptr;
std::chrono::steady_clock::time_point lastClickTime = std::chrono::steady_clock::now() - std::chrono::seconds(1);

std::wstring BluetoothAddressToString(uint64_t address) {
    std::wstringstream ss;
    ss << std::hex << std::setw(2) << std::setfill(L'0')
        << ((address >> 40) & 0xFF) << L":"
        << ((address >> 32) & 0xFF) << L":"
        << ((address >> 24) & 0xFF) << L":"
        << ((address >> 16) & 0xFF) << L":"
        << ((address >> 8) & 0xFF) << L":"
        << (address & 0xFF);
    return ss.str();
}

uint64_t StringToBluetoothAddress(const std::wstring& address) {
    uint64_t btAddr = 0;
    int shift = 40;
    for (size_t i = 0; i < address.length(); ++i) {
        if (address[i] != L':') {
            btAddr += std::stoul(address.substr(i, 2), nullptr, 16) << shift;
            shift -= 8;
            i++;
        }
    }
    return btAddr;
}

std::deque<std::pair<int, int>> kalHistory; // To store the last 10 kal[0] and kal[1] values

void HandleNotification(GattCharacteristic characteristic, GattValueChangedEventArgs args) {
    // static int preKal0 = 0;
    // static int preKal1 = 0;

    try {
        auto reader = DataReader::FromBuffer(args.CharacteristicValue());
        std::vector<uint8_t> raw_data;
        while (reader.UnconsumedBufferLength() > 0) {
            raw_data.push_back(reader.ReadByte());
        }

        if (raw_data.size() == 24) {
            int timestamp;
            int emgValues[3];
            int kal[2];
            memcpy(&timestamp, raw_data.data(), 4);
            memcpy(emgValues, raw_data.data() + 4, 12);
            memcpy(kal, raw_data.data() + 16, 8);
            std::wcout << timestamp << "\t" << emgValues[0] << "\t" << emgValues[1] << "\t" << emgValues[2] << "\t" << kal[0] << "\t" << kal[1] << "\t" << std::endl;

            // for stabilize
            // Store the current kal values in the history deque
            if (kalHistory.size() >= 5) {
                kalHistory.pop_front(); // Remove the oldest entry if we already have 10 values
            }
            kalHistory.push_back(std::make_pair(kal[0], kal[1]));

            // Calculate weighted average for kal[0] and kal[1] based on the last 10 values
            const std::vector<float> weights = { 0.1f, 0.15f, 0.2f, 0.25f, 0.3f }; // Weights for averaging
            float weightedKal0 = 0.0f, weightedKal1 = 0.0f;
            float totalWeight = 0.0f;

            // Apply weights
            int size = kalHistory.size();
            for (int i = 0; i < size; ++i) {
                float weight = weights[i % weights.size()];
                weightedKal0 += kalHistory[i].first * weight;
                weightedKal1 += kalHistory[i].second * weight;
                totalWeight += weight;
            }

            // Normalize the result
            weightedKal0 /= totalWeight;
            weightedKal1 /= totalWeight;

            // Move the mouse based on kal values
            INPUT input = { 0 };
            input.type = INPUT_MOUSE;
            input.mi.dx = static_cast<LONG>(weightedKal0 * 20);  // Adjust sensitivity
            input.mi.dy = static_cast<LONG>(-weightedKal1 * 40); // Adjust sensitivity
            input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
            SendInput(1, &input, sizeof(INPUT));

            // Check if any emgValue is greater than 2000 to trigger a mouse click
            auto now = std::chrono::steady_clock::now();
            if ((emgValues[0] > 1900 ||  emgValues[2] > 1900) &&
                (now - lastClickTime >= std::chrono::seconds(1))) {
                INPUT clickInput = { 0 };
                clickInput.type = INPUT_MOUSE;
                clickInput.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
                SendInput(1, &clickInput, sizeof(INPUT));

                clickInput.mi.dwFlags = MOUSEEVENTF_LEFTUP;
                SendInput(1, &clickInput, sizeof(INPUT));

                // Second click (for double click)
                clickInput.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
                SendInput(1, &clickInput, sizeof(INPUT));

                clickInput.mi.dwFlags = MOUSEEVENTF_LEFTUP;
                SendInput(1, &clickInput, sizeof(INPUT));

                lastClickTime = now; // Update the last click time
            }
        }
        else {
            std::wcerr << L"Received data of incorrect size." << std::endl;
        }
    }
    catch (const std::exception& e) {
        std::wcerr << L"Exception: " << e.what() << std::endl;
    }
    catch (...) {
        std::wcerr << L"Unknown exception occurred." << std::endl;
    }
}

void ReadBLEData(BluetoothLEDevice device) {
    try {
        std::wcout << "reading" << std::endl;
        running = true;
        auto services_result = device.GetGattServicesAsync().get();
        if (services_result.Status() != GattCommunicationStatus::Success) {
            std::wcerr << L"Failed to get GATT services." << std::endl;
            return;
        }

        for (auto service : services_result.Services()) {
            auto characteristics_result = service.GetCharacteristicsAsync().get();
            if (characteristics_result.Status() != GattCommunicationStatus::Success)
                continue;

            for (auto characteristic : characteristics_result.Characteristics()) {
                if (characteristic.Uuid() == winrt::guid(characteristic_uuid)) {
                    characteristic.ValueChanged({ &HandleNotification });
                    std::wcout << "valuechanged" << std::endl;

                    auto status = characteristic.WriteClientCharacteristicConfigurationDescriptorAsync(
                        GattClientCharacteristicConfigurationDescriptorValue::Notify
                    ).get();

                    if (status != GattCommunicationStatus::Success) {
                        std::wcerr << L"Failed to enable notifications." << std::endl;
                        return;
                    }

                    std::wcout << L"Started receiving notifications." << std::endl;

                    while (running) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }

                    std::wcout << "aftersleep" << std::endl;
                    characteristic.WriteClientCharacteristicConfigurationDescriptorAsync(
                        GattClientCharacteristicConfigurationDescriptorValue::None
                    ).get();
                    std::wcout << "writeasync2" << std::endl;
                    break;
                }
            }
        }
    }
    catch (const std::exception& e) {
        std::wcerr << L"Exception: " << e.what() << std::endl;
    }
    catch (...) {
        std::wcerr << L"Unknown exception occurred." << std::endl;
    }
}

void OnAdvertisementReceived(BluetoothLEAdvertisementWatcher watcher, BluetoothLEAdvertisementReceivedEventArgs args) {
    try {
        const std::wstring targetAddress = L"08:d1:f9:fd:40:7e";
        std::wcout << L"Device found: " << BluetoothAddressToString(args.BluetoothAddress()) << std::endl;

        if (BluetoothAddressToString(args.BluetoothAddress()) == targetAddress) {
            std::wcout << L"Target device found: " << BluetoothAddressToString(args.BluetoothAddress()) << std::endl;
            watcher.Stop();

            auto deviceTask = BluetoothLEDevice::FromBluetoothAddressAsync(args.BluetoothAddress());
            deviceTask.Completed([](IAsyncOperation<BluetoothLEDevice> const& asyncOp, AsyncStatus const status) {
                if (status == AsyncStatus::Completed) {
                    BluetoothLEDevice device = asyncOp.GetResults();
                    std::wcout << L"Connected to device: " << BluetoothAddressToString(device.BluetoothAddress()) << std::endl;
                    std::lock_guard<std::mutex> lock(device_mutex);
                    global_device = device;
                }
                else {
                    std::wcerr << L"Failed to connect to the device." << std::endl;
                }
                });
        }
    }
    catch (const std::exception& e) {
        std::wcerr << L"Exception: " << e.what() << std::endl;
    }
    catch (...) {
        std::wcerr << L"Unknown exception occurred." << std::endl;
    }
}

int main() {
    try {
        const std::wstring targetDeviceAddress = L"08:d1:f9:fd:40:7e";

        init_apartment();
        std::wcout << L"Waiting for connection..." << std::endl;
        BluetoothLEAdvertisementWatcher watcher;
        watcher.Received(OnAdvertisementReceived);
        watcher.ScanningMode(BluetoothLEScanningMode::Active);

        std::wcout << L"Starting BLE scan..." << std::endl;
        watcher.Start();
        std::this_thread::sleep_for(std::chrono::seconds(5));
        watcher.Stop();

        std::wcout << L"Type 'start' to begin reading BLE data: ";
        std::wstring input;
        std::wcin >> input;

        if (input == L"start") {
            std::wcout << L"Starting BLE data read" << std::endl;
            std::lock_guard<std::mutex> lock(device_mutex);
            if (global_device != nullptr) {
                std::thread ble_thread(ReadBLEData, global_device);
                ble_thread.join();
                std::wcout << L"Finished BLE data read" << std::endl;
            }
            else {
                std::wcerr << L"Failed to connect to the BLE device." << std::endl;
            }
        }
        else {
            std::wcout << L"Invalid input, exiting." << std::endl;
        }
    }
    catch (const std::exception& e) {
        std::wcerr << L"Exception: " << e.what() << std::endl;
    }
    catch (...) {
        std::wcerr << L"Unknown exception occurred." << std::endl;
    }

    return 0;
}