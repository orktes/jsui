// The built-in audio backend (src/audio/audio_builtin.cpp): a runtime
// without an audio handler playing sounds from a custom loader on
// miniaudio's null device (JSUI_AUDIO_BACKEND=null: no sound hardware
// needed). Ticks run on real time, since the null device plays in real time.
// Nothing to test when the library is built with JSUI_AUDIO OFF.
#if JSUI_AUDIO

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "jsui/jsui.h"
#include "test.h"

namespace {

// A mono 16-bit PCM WAV of `seconds` of a quiet sine.
std::string makeWav(double seconds, uint32_t rate = 44100) {
  uint32_t frames = uint32_t(seconds * rate);
  uint32_t dataSize = frames * 2;
  std::string w;
  auto u32 = [&](uint32_t v) {
    for (int i = 0; i < 4; i++) w += char((v >> (8 * i)) & 0xff);
  };
  auto u16 = [&](uint16_t v) {
    w += char(v & 0xff);
    w += char(v >> 8);
  };
  w += "RIFF";
  u32(36 + dataSize);
  w += "WAVEfmt ";
  u32(16);
  u16(1);  // PCM
  u16(1);  // mono
  u32(rate);
  u32(rate * 2);
  u16(2);
  u16(16);
  w += "data";
  u32(dataSize);
  for (uint32_t i = 0; i < frames; i++) u16(uint16_t(int16_t(2000 * std::sin(i * 2 * 3.14159265 * 440 / rate))));
  return w;
}

// 0.25 s of a 440 Hz sine, 22050 Hz mono (ffmpeg, libvorbis -q 0 / libmp3lame 32k).
const char* kOggBase64 =
    "T2dnUwACAAAAAAAAAAAAAAAAAAAAANoLuMsBHgF2b3JiaXMAAAAAASJWAAAAAAAAwF0AAAAAAACqAU9nZ1MAAAAAAAAAAAAAAAAAAAEAAABxjw"
    "5uDjD///////////////+aA3ZvcmJpcwYAAABmZm1wZWcBAAAAFgAAAGVuY29kZXI9TGF2YyBsaWJ2b3JiaXMBBXZvcmJpcyJCQ1YBAAgAAIAg"
    "ChnGgNCQVQAAEAAAQohGxlCnlASXgoUQR8RQh5DzUGrpIHhKYcmY9BRrEEII33vPvffeeyA0ZBUAAAQAQBgFDmLgMQlCCKEYxQlRnCkIQghhOQ"
    "mWch46CUL3IIQQLufecu699x4IDVkFAAACADAIIYQQQgghhBBCCimlFFKKKaaYYsoxxxxzzDHIIIMMOuikk04yqaSTjjLJqKPUWkotxRRTbLnF"
    "WGutNefca1DKGGOMMcYYY4wxxhhjjDHGCEJDVgEAIAAAhEEGGWQQQgghhRRSiimmHHPMMceA0JBVAAAgAIAAAAAAR5EUyZEcyZEkSbIkS9Ikz/"
    "Isz/IsTxM1UVNFVXVV27V925d923d12bd92XZ1WZdlWXdtW5d1V9d1Xdd1Xdd1Xdd1Xdd1Xdd1IDRkFQAgAQCgIzmOIzmOIzmSIymSAoSGrAIA"
    "ZAAABADgKI7iOJIjOZZjSZakSZrlWZ7laZ4maqIHhIasAgAAAQAEAAAAAACgKIriKI4jSZalaZrnqZ4oiqaqqqJpqqqqmqZpmqZpmqZpmqZpmq"
    "ZpmqZpmqZpmqZpmqZpmqZpmqZpmkBoyCoAQAIAQMdxHMdRHMdxHMmRJAkIDVkFAMgAAAgAwFAUR5Ecy7EkzdIsz/I00TM9V5RN3dRVGwgNWQUA"
    "AAIACAAAAAAAwPEcz/EcT/Ikz/Icz/EkT9I0TdM0TdM0TdM0TdM0TdM0TdM0TdM0TdM0TdM0TdM0TdM0TdM0TdM0TdOA0JBVAAACAAAgiEKGMS"
    "A0ZBUAAAQAgBCikTHUKSXBpWAhxBEx1CHkPJRaOgieUlgyJj3FGoQQwvfec++99x4IDVkFAAABABBGgYMYeEyCEEIoRnFCFGcKghBCWE6CpZyH"
    "ToLQPQghhMu5t5x7770HQkNWAQCAAAAMQgghhBBCCCGEkEJKKYWUYooppphyzDHHHHMMMsgggw466aSTTCrppKNMMuootZZSSzHFFFtuMdZaa8"
    "059xqUMsYYY4wxxhhjjDHGGGOMMYLQkFUAAAgAAGGQQQYZhBBCSCGFlGKKKcccc8wxIDRkFQAACAAgAAAAwFEkRXIkR3IkSZIsyZI0ybM8y7M8"
    "y9NETdRUUVVd1XZt3/Zl3/ZdXfZtX7ZdXdZlWdZd29Zl3dV1Xdd1Xdd1Xdd1Xdd1Xdd1HQgNWQUASAAA6EiO40iO40iO5EiKpAChIasAABkAAA"
    "EAOIqjOI7kSI7lWJIlaZJmeZZneZqniZroAaEhqwAAQAAAAQAAAAAAKIqiOIrjSJJlaZrmeaoniqKpqqpomqqqqqZpmqZpmqZpmqZpmqZpmqZp"
    "mqZpmqZpmqZpmqZpmqZpmiYQGrIKAJAAANBxHMdxFMdxHEdyJEkCQkNWAQAyAAACADAUxVEkx3IsSbM0y7M8TfRMzxVlUzd11QZCQ1YBAIAAAA"
    "IAAAAAAHA8x3M8x5M8ybM8x3M8yZM0TdM0TdM0TdM0TdM0TdM0TdM0TdM0TdM0TdM0TdM0TdM0TdM0TdM0TdM0IDRkJQAABACAIMe0gyQJhKCC"
    "5BnEHMSkGYWgguQ6BiXF5CGnoGLkOcmYQeSC0kWmIggNWREARAEAAMYgxhBzyDknpZMUOeekdFIaCKGljlJnqbRaYswoldpSrQ2EjlJILaNUYi"
    "2tdtRKrSW2AgAAAhwAAAIshEJDVgQAUQAAhDFIKaQUYow5yBxEjDHoGGSGMQYhc05BxxyFVCoHHXVQUsMYc45BqKCDVDpHlYNQUkedAACAAAcA"
    "gAALodCQFQFAnACAQZI0zdI0z7M0z/M8UVRVTxRV1RI90/RMU1U901RVUzVlV1RNWbY80TQ901RVzzRVVTRV2TVN1XU9VbVl01V1WXRV3XZt2b"
    "ddWRZuT1VlW1RdWzdVV9ZVWbZ9V7ZtXxJFVRVV1XU9VXVd1XV123RdXfdUVXZN15Vl03Vt2XVlW1dlWfg1VZVl03Vt2XRd2XZlV7dVWdZt0XV9"
    "XZVl4Tdl2fdlW9d9WbeVYXRd21dlWfdNWRZ+2ZaF3dV1X5hEUVU9VZVdUVVd13RdW1dd17Y11ZRd03Vt2VRdWVZlWfddV9Z1TVVl2ZRl2zZdV5"
    "ZVWfZ1V5Z1W3RdXTdlWfhVV9Z1V7eNY7ZtXxhdV/dNWdZ9VZZ1X9Z1YZh129c1VdV9U3Z94XRlXdh93xhmXReOz3V9X5Vt4Vhl2fh14ReWW9eF"
    "33NdX1dt2RhW2TaG3feNYfZ941h12xhmWze6uk4YfmE4bt84qrYtdHVbWF7dNurGT7iN36ipqq+brmv8piz7uqzbwnD7vnJ8ruv7qiwbvyrbwm"
    "/runLsvk/5XNcXVlkWhtWWhWHWdWHZhWGp2royvLpvHK+tK8PtC43fV4aqbRvLq9vCMPu28NvCbxy7sTMGAAAMOAAABJhQBgoNWREAxAkAWCTJ"
    "8yzLEkXLskRRNEVVFUVRVS1NM01N80xT0zzTNE1TdUXTVF1L00xT8zTT1DzNNE3VdFXTNGVTNE3XNVXTdkVVlWXVlWVZdV1dFk3TlUXVdGXTVF"
    "1ZdV1XVl1XliVNM03N80xT8zzTNFXTlU1TdV3L81RT80TT9URRVVVTVV1TVWVX8zxT9URPNT1RVFXTNWXVVFVZNlXTlk1TlWXTVW3ZVWVXll3Z"
    "tk1VlWVTNV3ZdF3Xdl3Xdl3ZFXZJ00xT8zzT1DxPNU1TdV1TVV3Z8jzV9ERRVTVPNFVVVV3XNFVXtjzPVD1RVFVN1FTTdF1ZVlVTVkXVtGVVVX"
    "XZNFVZdmXZtl3VdWVTVV3ZVF1ZNlVTdl1XtrmyKqueacqyqaq2bKqq7Mq2beuu6+q2qJqya5qqbKuqqruya+u+LMu2LKqq65quKsumqsq2LMu6"
    "Lsu2sKuua9um6sq6K8t0WbVd3/Ztuuq6tq/Krq+7smzrru3qsm7bvu+ZpiybqinbpqrKsiy7tm3Lsi+Mpunapqvasqm6su26rq7LsmzbomnKsq"
    "m6rm2qpizLsmz7sizbturKuuzasu27rizbsm0Lu+wKs6+6sq27sm0Lq6vatuzbPltXdVUAAMCAAwBAgAlloNCQlQBAFAAAYAxjjEFolHLOOQiN"
    "Us45ByFzDkIIqWTOQQihpMw5CKWklDkHoZSUQgilpNRaCKGUlForAACgwAEAIMAGTYnFAQoNWQkApAIAGBxH00zTdWXZGBbLEkVVlWXbNobFsk"
    "RRVWXZtoVjE0VVlWXb1nU0UVRVWbZt3VeOU1Vl2bZ9XTgyVVWWbVvXfSNVlm1b14WhkirLtm3rvlFJtm1dN4bjqCTbtu77vnEs8YWhsCyV8JVf"
    "OCqBAADwBAcAoAIbVkc4KRoLLDRkJQCQAQAAGKSUUUopo5RSSinGlFKMCQAAGHAAAAgwoQwUGrIiAIgCAACcc84555xzzjnnnHPOOeecc8455x"
    "hjjDHGGGOMMcYYY4wxxhhjjDHGGGOMMcYYY4wxxgQA7EQ4AOxEWAiFhqwEAMIBAACEFIKSUimllBI556SUUkoppZTIQQillFJKKaVE0kkppZRS"
    "SimlcVBKKaWUUkopoZRSSimllFJKCaWUUkoppZRSSimllFJKKaWUUkoppZRSSimllFJKKaWUUkoppZRSSimllFJKKaWUUkoppZRSSimllFJKKa"
    "WUUkoppZRSSimllFJKKaWUUkoppZRSSgEAJg8OAFAJNs6wknRWOBpcaMhKACA3AABQijnGJJSQSkglhBBK5RiEzkkJKbVWQgqthAo6aJ2jkFJL"
    "rZWUSkmZhBBCKKGEUlopJbVSMgihhFBKCCGlUkoJoWVQQgollJRSSS20VErJIIRQWgmpldRaCiWVlEEpqYSSUiqttZRKSq2D0lIprbXWSkohlZ"
    "ZSB6WkllIppbUWSmuttU5SKS2k1lJrrZVWSimdpZRKSa21llprKaVWQimttNJaKSW11lJrLZXUWkutpdZSa62l1kopJaWWWmuttZZaKim1lEIp"
    "pZWSQmqppdZKKi2E0FJJpZVWWmsppZRKKCWVlFoqqbWWUmilhdJKSSWllkoqKaXUUiqhlBJSKqGV1FJrqaWWSiottdRSK6mUlkpKqRQAAHTgAA"
    "AQYESlhdhpxpVH4IhChgkoAAAQBAAYiJCZQKAACgxkAMABQoIUAFBYYChd6IIQIkgXQRYPXDhx44kbTujQBgAYiJCZAKEYIiRkA8AERYV0ALC4"
    "wChd6IIQIkgXQRYPXDhx44kbTujQAgEAAAAAAAIAHwAABwYQEdFchsYGR4fHB0iICAAAAAAAAAAAAAAAgE9nZ1MABIkVAAAAAAAAAAAAAAIAAA"
    "DqIl9CDBoXFxcWFxgXGBgXIDpX7mhSAMqjcAQBbf8JAQAAgMjzPFK0z1cDSl3JTlYAydegGgEAAACAkgCAdEcfIgZKXclO6wCS16AaAQAAAIBR"
    "EwDUlllHCEpdyU6pAJJvgGoEAAAAAKQEAExhnXEASl3JTusAktegGgEAAACALQCg3WP3AEpdyU5WAMnXoBoBAAAAACkAQPnHLYwBSl3JTqkAko"
    "9BNQIAAAAAow4AqP32FkYASl3JjkcAyRehGgEAAACAUhcApPl4Ig5KXcleVgDJF6EaAQAAAICWFgAo8/FXHABKXclOqQCS16AaAQAAAIBNBwDt"
    "a1jGAABGXWGHACA+DaoRAAAAAJAAlGGKjSaKAS5V7t6M0E55FIEjgHW1VxZAGAAwv6Of5TnzvmkFaMAD";
const char* kMp3Base64 =
    "SUQzBAAAAAAAI1RTU0UAAAAPAAADTGF2ZjYwLjE2LjEwMAAAAAAAAAAAAAAA//NwwAAAAAAAAAAAAEluZm8AAAAPAAAADAAABZwAMjIyMjIyMj"
    "JFRUVFRUVFRVhYWFhYWFhYampqampqampqfX19fX19fX2QkJCQkJCQkKKioqKioqKiorW1tbW1tbW1yMjIyMjIyMja2tra2tra2trt7e3t7e3t"
    "7f//////////AAAAAExhdmM2MC4zMQAAAAAAAAAAAAAAACQDNwAAAAAAAAWcNZ7kvAAAAAAAAAAAAAAAAAD/80DEABMwYoQ/TxgAC9bbtgh5pm"
    "maZpoe5sCGEHELAdwCMA7BVjjOtnePHjx48BAEwfB8H+c4P+GJT38p5/lPP8p7+jggD4PvB8HAQDCgwD4Py4EBDg+/R70gCSCDDAAAvMAgNpP3"
    "Jv/zQsQOF/FyfNWcaADiYxGBgp9tYMoC4oChiqbGnQYYLBglalQGSQg5IOkFdBWfxJgvQXb8dowowpO/xhhhiaPUev/mReJIxLpdS//y8XjEul"
    "1IvF47/lQkDQlCQNCVEkkbkckH7xxkiv/zQMQKELhWWH/dAAIEFw2MBQkAQnGC4vGSY8H26omTQ3lAgNeQlPO/sar2dltDVVq+mt3/iuBvt/v/"
    "s/fs/os7em+/+9wia+/96qtGMBUCey+qmQKAAjALAB83lwmuA4iLDyoXmk0+//NCxCIVOPoYSv7EZHRUajU7a32ezr6dFX4j6OFyw7t5VTBUmf"
    "V5Vc7LxZ4kOMWk49x5U+hzrCVFLl/FtTXpN/3q7NLZMCJDOgqADF/TACAAMwEMA4OcjOLTuQgONUImgwqcEBGqV307//NAxCkU+VYQAP7KZGj2"
    "v3178+QcRmZ+7fTrhqL8tcoJvgb6lLKXkl129iVO6kk9A3t1xfVTe579Y6mWFGQllhYA0TjABQAAwDYA3OIQNwjngcOKkzW+k0/mKvvVWXsoo2"
    "phlXpVjcn/80LEMBEwThQA7/ZA6vIx2/qtpU6cu+zWuE2bLMQV/V5S4AASHd/dtv8J8wLMrVaVnRpakFpNTtrjxXGil29t/r5H/1v2+r/sR6v0"
    "Yv+v/1VSP9FBP1zKq3YwIoJjLmpDAIACMAqAOTf/80DERwx4QlJ+DrwgUw5COEBw4aV680htnS7U7b/2f//T4uz9Ua5v6U9v9ra7KOtmz4y9Fs"
    "w+2hf2boDlrr/1vJ4TNvKS+WFLwmANgFBrJZeKd0ALBWbQLPXhVmHqdbrL1bV0s/RH3f/zQsRwEMD6GCL+xGT+j/7tbl39W4h7V6v/+5J/5GwB"
    "HgwAoAKBoBKYDIDRjIIGYMqComEIBaRhaAA4al4UlGFtgapgkAKAYGEA+GA6AHhgCYBmTABysokAFrEln//8//93///ZVV/21f/zQMSJDvhKIO"
    "rn9EAkABNCMRiQWCM4AC6h0MwKgsQioAGBwsEmf9RppWYrUmBDYEDztAJNcD0pi4CPABeIiBm4DWkDMjxOwfsLeGIeAUnAgCAaCCuilRXvDIAj"
    "UMTiPThAjcy+Hrkw//NCxKgTCFogAV8QACfxZg7DMc0mSqQ7+K0ICJ3ICOMihCmReOqMTUx/5FDxOHSfOny4bMZKSXRZf/80miYekCNHndV0kE"
    "wBEZ0GkgQJR8zROkVfmSJ2mbAs0L3Fki2yunqQTFpkfoWy//NAxLckYgJ5vZugAZXaxF+YRBAiIqloyMj7mlz31rNpta1v1lb7K1bWbNPAIGn/"
    "EqwVcIjwiPf+lUxBTUUzLjEwMFVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVX/80LEgBRg/cgB2GAAVVVVVVVVVVVVVVVVVV"
    "VVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVVU=";

struct BuiltinHost {
  JsuiRuntime* rt = nullptr;
  std::vector<std::string> events;  // "loaded <url> <ok> <duration>", "ended <voice> <status>"
  std::vector<std::string> loads;
  std::string shortWav = makeWav(0.1), longWav = makeWav(3), musicWav = makeWav(21, 8000);
  std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();

  explicit BuiltinHost(const char* backend = "null") {
#ifdef _WIN32
    _putenv_s("JSUI_AUDIO_BACKEND", backend);
#else
    setenv("JSUI_AUDIO_BACKEND", backend, 1);
#endif
    JsuiConfig cfg;
    jsui_config_init(&cfg);
    cfg.width = 32;
    cfg.height = 32;
    cfg.base_url = "http://game.test/ui/bundle.js";
    cfg.on_message = [](void* u, const char* name, const char* json) {
      std::string s = json ? json : "";
      if (s.size() >= 2 && s.front() == '"') s = s.substr(1, s.size() - 2);  // JSON string payloads
      static_cast<BuiltinHost*>(u)->events.push_back(std::string(name) + " " + s);
    };
    cfg.message_user = this;
    cfg.load = [](void* u, JsuiRuntime* rt, uint64_t id, const char* url) {
      auto* h = static_cast<BuiltinHost*>(u);
      std::string path(url);
      h->loads.push_back(path);
      const std::string* body = nullptr;
      std::string garbage(4096, '\x5a');
      if (path.find("short.wav") != std::string::npos) body = &h->shortWav;
      else if (path.find("long.wav") != std::string::npos) body = &h->longWav;
      else if (path.find("music.wav") != std::string::npos) body = &h->musicWav;
      else if (path.find("garbage.wav") != std::string::npos) body = &garbage;
      if (body) jsui_resource_complete(rt, id, 200, "audio/wav", body->data(), body->size());
      else jsui_resource_complete(rt, id, 404, nullptr, nullptr, 0);
    };
    cfg.load_user = this;
    rt = jsui_create(&cfg);
    std::string bundle = R"(
      const N = __jsui_native;
      globalThis.__jsui_onAudio = (events) => {
        for (const e of events) {
          const url = typeof e[1] === 'string' && e[1].startsWith('data:') ? e[1].slice(0, e[1].indexOf(';')) : e[1];
          if (e[0] === 1) N.postMessage('loaded', url.replace('http://game.test/ui/', '') + ' ' + e[2] + ' ' + e[3].toFixed(2));
          else N.postMessage('ended', e[1] + ' ' + e[2]);
        }
      };
      globalThis.__jsui_onHostMessage = (name, json) => { if (name === 'run') (0, eval)(JSON.parse(json)); };
      globalThis.play = (url, bus, loop, offset) => N.audioPlay(url, bus || 'sfx', 1, 1, 0, !!loop, 0, offset || 0);
    )";
    jsui_load_bundle_source(rt, bundle.data(), bundle.size(), "bundle.js");
  }
  ~BuiltinHost() {
    if (rt) jsui_destroy(rt);
  }
  // Runs `js` (no double quotes or backslashes) in the bundle's context.
  void run(const std::string& js) { jsui_post_message(rt, "run", ("\"" + js + "\"").c_str()); }
  void tick() {
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    jsui_tick(rt, ms);
  }
  bool has(const std::string& e) const {
    for (auto& x : events)
      if (x == e) return true;
    return false;
  }
  // Ticks (on real time) until `done` or `ms` pass.
  bool until(const std::function<bool()>& done, int ms = 3000) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    for (;;) {
      tick();
      if (done()) return true;
      if (std::chrono::steady_clock::now() > end) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  bool waitFor(const std::string& e, int ms = 3000) {
    return until([&] { return has(e); }, ms);
  }
};

}  // namespace

TEST(audio_builtin_preload_reports_the_duration) {
  BuiltinHost h;
  h.run("N.audioPreload('short.wav'); N.audioPreload('long.wav'); N.audioPreload('short.wav');");
  CHECK(h.waitFor("loaded short.wav 1 0.10"));
  CHECK(h.waitFor("loaded long.wav 1 3.00"));
  int shortLoads = 0, shortAnswers = 0;
  for (auto& u : h.loads) shortLoads += u.find("short.wav") != std::string::npos;
  for (auto& e : h.events) shortAnswers += e == "loaded short.wav 1 0.10";
  CHECK_EQ(shortLoads, 1);    // read once
  CHECK_EQ(shortAnswers, 2);  // every PRELOAD answered
  // Loaded already: answered at once, without another read.
  h.run("N.audioPreload('short.wav');");
  CHECK(h.until([&] {
    int n = 0;
    for (auto& e : h.events) n += e == "loaded short.wav 1 0.10";
    return n == 3;
  }));
  CHECK_EQ(h.loads.size(), size_t(2));
}

TEST(audio_builtin_decodes_ogg_vorbis_and_mp3) {
  BuiltinHost h;
  // data: URIs, decoded by the loader.
  h.run(std::string("N.audioPreload('data:audio/ogg;base64,") + kOggBase64 + "'); play('data:audio/mpeg;base64," +
        kMp3Base64 + "');");
  CHECK(h.waitFor("loaded data:audio/ogg 1 0.25"));
  CHECK(h.waitFor("ended 1 0"));
}

TEST(audio_builtin_short_sound_finishes_after_playing) {
  BuiltinHost h;
  auto start = std::chrono::steady_clock::now();
  h.run("play('short.wav');");  // voice 1, before its bytes are there
  CHECK(h.waitFor("ended 1 0"));  // JSUI_AUDIO_FINISHED
  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  CHECK(ms >= 80);  // it played (100 ms of sound)
  CHECK(!h.has("ended 1 1") && !h.has("ended 1 -1"));
}

TEST(audio_builtin_long_sounds_stream_from_an_offset) {
  BuiltinHost h;
  // 21 s: streamed (decoded while it plays), started 0.1 s before its end.
  h.run("N.audioPreload('music.wav'); play('music.wav', 'music', false, 20.9);");
  CHECK(h.waitFor("loaded music.wav 1 21.00"));
  CHECK(h.waitFor("ended 1 0", 1500));
}

TEST(audio_builtin_without_a_device_fails_every_voice) {
  BuiltinHost h("no-such-backend");
  h.run("play('short.wav'); N.audioPreload('short.wav'); play('long.wav');");
  CHECK(h.waitFor("ended 1 -1"));
  CHECK(h.waitFor("loaded short.wav 0 -1.00"));
  CHECK(h.waitFor("ended 2 -1"));
  CHECK_EQ(h.loads.size(), size_t(0));
}

TEST(audio_builtin_stop_pause_and_set) {
  BuiltinHost h;
  h.run("play('long.wav'); play('long.wav', 'sfx', true);");
  h.until([] { return false; }, 100);
  h.run("N.audioVoice(6, 1, 0.5, 1.5, -0.5, 50); N.audioVoice(4, 2, 1, 1, 0, 0);");
  h.until([] { return false; }, 100);
  h.run("N.audioVoice(5, 2, 1, 1, 0, 0); N.audioVoice(3, 1, 1, 1, 0, 100); N.audioVoice(3, 2, 1, 1, 0, 0);");
  CHECK(h.waitFor("ended 1 1"));  // STOPPED after its fade out
  CHECK(h.waitFor("ended 2 1"));
  CHECK(!h.has("ended 1 0") && !h.has("ended 2 0"));
}

TEST(audio_builtin_bad_sounds_fail) {
  BuiltinHost h;
  h.run("play('garbage.wav'); play('missing.wav'); N.audioPreload('missing.wav'); N.audioPreload('garbage.wav');");
  CHECK(h.waitFor("ended 1 -1"));
  CHECK(h.waitFor("ended 2 -1"));
  CHECK(h.waitFor("loaded missing.wav 0 -1.00"));
  CHECK(h.waitFor("loaded garbage.wav 0 -1.00"));
  // A failed sound is read again by a later request.
  size_t reads = h.loads.size();
  h.run("play('missing.wav');");
  CHECK(h.waitFor("ended 3 -1"));
  CHECK_EQ(h.loads.size(), reads + 1);
}

TEST(audio_builtin_stop_all_by_bus) {
  BuiltinHost h;
  h.run("play('long.wav', 'music', true); play('long.wav', 'music'); play('long.wav', 'ui', true);");
  h.until([] { return false; }, 100);
  h.run("N.audioBus('music', 0.5, false, 100); N.audioBus('ui', 1, true, 0); N.audioStopAll('music', 0);");
  CHECK(h.waitFor("ended 1 1"));
  CHECK(h.waitFor("ended 2 1"));
  h.until([] { return false; }, 100);
  CHECK(!h.has("ended 3 1"));  // another bus: still playing
  h.run("N.audioStopAll(null, 50);");
  CHECK(h.waitFor("ended 3 1"));
}

TEST(audio_builtin_unload_and_destroy_while_playing) {
  BuiltinHost h;
  h.run("play('long.wav', 'music', true); N.audioPreload('short.wav');");
  CHECK(h.waitFor("loaded short.wav 1 0.10"));
  h.run("N.audioUnload('short.wav'); N.audioUnload('long.wav'); play('short.wav');");
  CHECK(h.waitFor("ended 2 0"));
  int shortLoads = 0;
  for (auto& u : h.loads) shortLoads += u.find("short.wav") != std::string::npos;
  CHECK_EQ(shortLoads, 2);  // unloaded: read again
  CHECK(!h.has("ended 1 0"));  // still looping; jsui_destroy stops it
}

#endif  // JSUI_AUDIO
