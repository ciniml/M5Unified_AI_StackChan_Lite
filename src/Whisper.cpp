#include <ArduinoJson.h>
#include "Whisper.h"

#define USE_AAC_ENCODER
#ifdef USE_AAC_ENCODER
#include "esp_aac_enc.h"
#include "esp_audio_enc_def.h"
#include "esp_audio_def.h"
#include "aacmp4.hpp"
#include <vector>
#endif

namespace {
constexpr char* API_HOST = "api.openai.com";
constexpr int API_PORT = 443;
constexpr char* API_PATH = "/v1/audio/transcriptions";
}  // namespace

template<typename TClient>
struct ClientStreamAdapter {
  TClient& client;
  ClientStreamAdapter(TClient& client) : client(client) {}
  void write(const uint8_t* data, size_t size) {
    size_t bytes_written = 0;
    while( bytes_written < size ) {
      auto written = client.write(data + bytes_written, size - bytes_written);
      bytes_written += written;
    }
  }
};

Whisper::Whisper(const char* root_ca, const char* api_key) : client(), key(api_key) {
  client.setCACert(root_ca);
  client.setTimeout(10000); 
  if (!client.connect(API_HOST, API_PORT)) {
    Serial.println("Connection failed!");
  }
}

Whisper::~Whisper() {
  client.stop();
}

String Whisper::Transcribe(AudioWhisper* audio) {
  char boundary[64] = "------------------------";
  for (auto i = 0; i < 2; ++i) {
    ltoa(random(0x7fffffff), boundary + strlen(boundary), 16);
  }
  const String header = "--" + String(boundary) + "\r\n"
    "Content-Disposition: form-data; name=\"model\"\r\n\r\nwhisper-1\r\n"
    "--" + String(boundary) + "\r\n"
    "Content-Disposition: form-data; name=\"language\"\r\n\r\nja\r\n"
    "--" + String(boundary) + "\r\n"
#ifdef USE_AAC_ENCODER
    "Content-Disposition: form-data; name=\"file\"; filename=\"speak.mp4\"\r\n"
    "Content-Type: application/octet-stream\r\n\r\n";
#else
    "Content-Disposition: form-data; name=\"file\"; filename=\"speak.wav\"\r\n"
    "Content-Type: application/octet-stream\r\n\r\n";
#endif
  const String footer = "\r\n--" + String(boundary) + "--\r\n";

  // header
  client.printf("POST %s HTTP/1.1\n", API_PATH);
  client.printf("Host: %s\n", API_HOST);
  client.println("Accept: */*");
  client.printf("Authorization: Bearer %s\n", key.c_str());

  auto send_start_time = esp_timer_get_time();
  auto ptr = audio->GetBuffer();
  auto remainings = audio->GetSize();
#ifdef USE_AAC_ENCODER
  {
    void* enc_handle = nullptr;
    esp_audio_err_t ret;
    esp_aac_enc_config_t config = ESP_AAC_ENC_CONFIG_DEFAULT();
    config.sample_rate = 16000;
    config.channel = 1;
    config.bitrate = 12000;
    config.adts_used = 0;

    // Skip RIFF WAV header (44 bytes)
    remainings -= 44;
    ptr += 44;

    // Create encoder handle
    ret = esp_aac_enc_open(&config, sizeof(esp_aac_enc_config_t), &enc_handle);
    if (ret != 0) {
        printf("Fail to create encoder handle.");
    } else {
      // Get in/out buffer size and malloc in/out buffer
      size_t input_offset = 0;
      int in_frame_size = 0;
      int out_frame_size = 0;
      ret = esp_aac_enc_get_frame_size(enc_handle, &in_frame_size, &out_frame_size);
      std::vector<uint8_t> in_buf(in_frame_size);
      std::vector<AACMP4::u32> chunks;
      chunks.reserve(128);
      std::vector<uint8_t> out_buffer;
      out_buffer.reserve(out_frame_size * chunks.size());
      
      // Encode audio data and send.
      while( remainings > 0) {
        esp_audio_enc_in_frame_t in_frame = { 0 };
        esp_audio_enc_out_frame_t out_frame = { 0 };
        in_frame.buffer = const_cast<std::uint8_t*>(reinterpret_cast<const std::uint8_t*>(ptr)) + input_offset;
        in_frame.len = in_frame_size;

        // Extend the output buffer 
        size_t out_offset = out_buffer.size();
        out_buffer.resize(out_offset + out_frame_size);
        out_frame.buffer = out_buffer.data() + out_offset;
        out_frame.len = out_frame_size;
        
        auto bytes_processed = in_frame_size;
        if( remainings < in_frame_size ) {
          // Copy the remaining data to the buffer and add padding.
          std::copy(ptr, ptr + remainings, in_buf.begin());
          std::fill(in_buf.begin() + remainings, in_buf.end(), 0);
          in_frame.buffer = in_buf.data();
          bytes_processed = remainings;
          remainings = 0;
        } else {
          remainings -= in_frame_size;
        }
        const auto samples_processed = bytes_processed / sizeof(int16_t);

        ret = esp_aac_enc_process(enc_handle, &in_frame, &out_frame);
        if (ret != ESP_AUDIO_ERR_OK) {
            printf("audio encoder process failed.\n");
            break;
        }

        input_offset += in_frame_size;
        if(out_frame.encoded_bytes == 0) continue;
        chunks.push_back(out_frame.encoded_bytes);
        out_buffer.resize(out_offset + out_frame.encoded_bytes);
        out_offset += out_frame.encoded_bytes;
      }
      esp_aac_enc_close(enc_handle);

      auto encode_end_time = esp_timer_get_time();

      AACMP4::DummyWriter dummy_writer;
      // Calculate output size.
      AACMP4::write_aac_mp4(dummy_writer, chunks, out_buffer, 16000, input_offset / 2, in_frame_size / 2);
      // for debug
      // listen with `nc -l 12345` on the host machine to receive the encoded audio file.
      // "192.168.2.14" must be replaced with the address of the host machine.
      // {
      //   WiFiClient debugClient;
      //   if( debugClient.connect("192.168.2.14", 12345) ) {
      //     //debugClient.write(out_buffer.data(), out_buffer.size());
      //     AACMP4::write_aac_mp4(debugClient, chunks, out_buffer, 16000, input_offset / 2, in_frame_size / 2);
      //     debugClient.flush();
      //   } else {
      //     log_e("Failed to connect to debug server.");
      //   }
      // }

      log_w("Input samples: %lu, Frame size: %lu, Output size: %d, elapsed: %lu", input_offset/2, in_frame_size/2, dummy_writer.bytes_written, encode_end_time - send_start_time);
      client.printf("Content-Length: %d\n", header.length() + dummy_writer.bytes_written + footer.length());
      client.printf("Content-Type: multipart/form-data; boundary=%s\n", boundary);
      client.println();
      client.print(header.c_str());
      client.flush();
      
      ClientStreamAdapter adapter(client);
      AACMP4::write_aac_mp4(adapter, chunks, out_buffer, 16000, input_offset / 2, in_frame_size / 2);
      client.flush();
    }
  }
#else
  client.printf("Content-Length: %d\n", header.length() + audio->GetSize() + footer.length());
  client.printf("Content-Type: multipart/form-data; boundary=%s\n", boundary);
  client.println();
  client.print(header.c_str());
  client.flush();
  while (remainings > 0) {
    auto sz = (remainings > 512) ? 512 : remainings;
    client.write(ptr, sz);
    client.flush();
    remainings -= sz;
    ptr += sz;
  }
#endif
  client.flush();

  auto send_elapsed_time = esp_timer_get_time() - send_start_time;
  printf("Send elapsed time: %lld us\n", send_elapsed_time);  

  // footer
  client.print(footer.c_str());
  client.flush();

  // wait response
  const auto now = ::millis();
  while (client.available() == 0) {
    if (::millis() - now > 10000) {
      Serial.println(">>> Client Timeout !");
      return "";
    }
  }

  bool isBody = false;
  String body = "";
  while(client.available()){
    const auto line = client.readStringUntil('\r');
    if (isBody) {
      body += line;
    } else if (line.equals("\n")) {
      isBody = true;
    }
  }

  StaticJsonDocument<200> doc;
  ::deserializeJson(doc, body);
  return doc["text"].as<String>();
}
