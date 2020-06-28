#include "cyton_daisy_wifi.h"
#include "custom_cast.h"
#include "timestamp.h"

#ifndef _WIN32
#include <errno.h>
#endif

#define START_BYTE 0xA0
#define END_BYTE_STANDARD 0xC0
#define END_BYTE_ANALOG 0xC1
#define END_BYTE_MAX 0xC6


// load default settings for cyton boards
int CytonDaisyWifi::prepare_session ()
{
    int res = OpenBCIWifiShieldBoard::prepare_session ();
    if (res != (int)BrainFlowExitCodes::STATUS_OK)
    {
        return res;
    }
    return config_board ("d");
}

void CytonDaisyWifi::read_thread ()
{
    // format is the same as for cyton but need to join two packages together
    /*
        Byte 1: 0xA0
        Byte 2: Sample Number
        Bytes 3-5: Data value for EEG channel 1
        Bytes 6-8: Data value for EEG channel 2
        Bytes 9-11: Data value for EEG channel 3
        Bytes 12-14: Data value for EEG channel 4
        Bytes 15-17: Data value for EEG channel 5
        Bytes 18-20: Data value for EEG channel 6
        Bytes 21-23: Data value for EEG channel 6
        Bytes 24-26: Data value for EEG channel 8
        Aux Data Bytes 27-32: 6 bytes of data
        Byte 33: 0xCX where X is 0-F in hex
    */
    int res;
    unsigned char b[OpenBCIWifiShieldBoard::package_size];
    double accel[3] = {0.};
    while (keep_alive)
    {
        res = server_socket->recv (b, OpenBCIWifiShieldBoard::package_size);
        if (res != OpenBCIWifiShieldBoard::package_size)
        {
            safe_logger (spdlog::level::warn, "recv result: {}", res);
            if (res == -1)
            {
#ifdef _WIN32
                safe_logger (spdlog::level::warn, "WSAGetLastError is {}", WSAGetLastError ());
#else
                safe_logger (spdlog::level::warn, "errno {} message {}", errno, strerror (errno));
#endif
            }

            continue;
        }

        double package[30] = {0.};
        bool first_sample = false;
        if (b[0] != START_BYTE)
        {
            break; // drop entire transaction for daisy
        }
        unsigned char *bytes = b + 1; // for better consistency between plain cyton and wifi, in
                                      // plain cyton index is shifted by 1

        if ((bytes[31] < END_BYTE_STANDARD) || (bytes[31] > END_BYTE_MAX))
        {
            safe_logger (spdlog::level::warn, "Wrong end byte {}", bytes[31]);
            continue;
        }

        // place unprocessed bytes to other_channels for all modes
        if ((bytes[0] % 2 == 0) && (first_sample))
        {
            // eeg
            for (int i = 0; i < 8; i++)
            {
                package[i + 9] = eeg_scale * cast_24bit_to_int32 (bytes + 1 + 3 * i);
            }
            // need to average other_channels
            package[21] += (double)bytes[25];
            package[22] += (double)bytes[28];
            package[23] += (double)bytes[27];
            package[24] += (double)bytes[28];
            package[25] += (double)bytes[29];
            package[26] += (double)bytes[30];
            package[21] /= 2.0;
            package[22] /= 2.0;
            package[23] /= 2.0;
            package[24] /= 2.0;
            package[25] /= 2.0;
            package[26] /= 2.0;
            package[20] = (double)bytes[31];
        }
        else
        {
            first_sample = true;
            package[0] = (double)bytes[0];
            // eeg
            for (int i = 0; i < 8; i++)
            {
                package[i + 1] = eeg_scale * cast_24bit_to_int32 (bytes + 1 + 3 * i);
            }
            // other_channels
            package[21] = (double)bytes[25];
            package[22] = (double)bytes[26];
            package[23] = (double)bytes[27];
            package[24] = (double)bytes[28];
            package[25] = (double)bytes[29];
            package[26] = (double)bytes[30];
        }

        // place processed accel data
        if (bytes[31] == END_BYTE_STANDARD)
        {
            int32_t accel_temp[3] = {0};
            accel_temp[0] = cast_16bit_to_int32 (bytes + 25);
            accel_temp[1] = cast_16bit_to_int32 (bytes + 27);
            accel_temp[2] = cast_16bit_to_int32 (bytes + 29);

            if ((bytes[0] % 2 == 0) && (first_sample))
            {
                // need to average accel data
                if (accel_temp[0] != 0)
                {
                    accel[0] += accel_scale * accel_temp[0];
                    accel[1] += accel_scale * accel_temp[1];
                    accel[2] += accel_scale * accel_temp[2];

                    accel[0] /= 2.f;
                    accel[1] /= 2.f;
                    accel[2] /= 2.f;
                }

                package[20] = (double)bytes[31];
            }
            else
            {
                first_sample = true;
                package[0] = (double)bytes[0];

                // accel
                if (accel_temp[0] != 0)
                {
                    accel[0] = accel_scale * accel_temp[0];
                    accel[1] = accel_scale * accel_temp[1];
                    accel[2] = accel_scale * accel_temp[2];
                }
            }

            package[17] = accel[0];
            package[18] = accel[1];
            package[19] = accel[2];
        }
        // place processed analog data
        if (bytes[31] == END_BYTE_ANALOG)
        {
            if ((bytes[0] % 2 == 0) && (first_sample))
            {
                // need to average analog data
                package[27] += cast_16bit_to_int32 (bytes + 25);
                package[28] += cast_16bit_to_int32 (bytes + 27);
                package[29] += cast_16bit_to_int32 (bytes + 29);
                package[27] /= 2.0f;
                package[28] /= 2.0f;
                package[29] /= 2.0f;
                package[20] = (double)bytes[31]; // cyton end byte
            }
            else
            {
                first_sample = true;
                package[0] = (double)bytes[0];
                // analog
                package[27] = cast_16bit_to_int32 (bytes + 25);
                package[28] = cast_16bit_to_int32 (bytes + 27);
                package[29] = cast_16bit_to_int32 (bytes + 29);
            }
        }
        // commit package
        if ((bytes[0] % 2 == 0) && (first_sample))
        {
            double timestamp = get_timestamp ();
            db->add_data (timestamp, package);
            streamer->stream_data (package, 30, timestamp);
        }
    }
}
