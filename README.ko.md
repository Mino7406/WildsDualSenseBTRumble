# Wilds DualSense BT Rumble

*[Read in English](README.md) · [Nexus Mods 페이지](https://www.nexusmods.com/monsterhunterwilds/mods/4944)*

몬스터 헌터 와일즈에서 듀얼센스를 블루투스로 연결했을 때 사라지는 진동을 되살립니다.

## 문제

와일즈는 듀얼센스에 "고급 햅틱" 경로로 진동을 보냅니다. 이 햅틱은 패드를 USB로 연결했을 때만 생기는 오디오 엔드포인트를 통해 보이스코일로 전달됩니다. **블루투스에는 그 엔드포인트 자체가 없고, 와일즈에는 일반 진동으로 대체하는 경로도 없어서** 무선으로 플레이하면 진동이 완전히 사라집니다. 다른 게임들이 멀쩡한 이유는 대부분 일반 진동을 보내기 때문이고, 그건 블루투스로도 잘 전달됩니다.

## 하는 일

캡콤이 이미 만들어둔 모터 파형 — 세기, 지속 시간, 어느 모터를 쓸지, 페이드아웃 여부 — 을 그대로 읽어서 블루투스로 직접 패드에 보냅니다.

비슷하게 흉내 낸 게 아닙니다. 모든 무기, 모든 상황에서 게임 자체의 진동 데이터를 게임 자체의 이벤트에 맞춰 그대로 재생합니다. 이 모드는 게임을 관찰하기만 하며, 게임 동작을 바꾸거나 값을 덮어쓰지 않습니다. 원래 조용했던 경로에 전달을 하나 추가할 뿐입니다.

## 설치

릴리즈를 받아 몬스터 헌터 와일즈 폴더에 풀어주세요:

```
MonsterHunterWilds/reframework/autorun/WildsDualSenseBTRumble.lua
MonsterHunterWilds/reframework/plugins/WildsDualSenseBTRumble.dll
```

[REFramework](https://github.com/praydog/REFramework)와 블루투스로 연결된 듀얼센스가 필요합니다. 따로 실행할 프로그램도, 설치할 런타임도 없습니다.

## 동작 원리

두 부분으로 나뉘어 있고, 둘 중 하나만으로는 동작하지 않습니다.

**`lua/WildsDualSenseBTRumble.lua`** 는 `ace.PadVibrationManager<app.cADVibration>.tryADVibration` 을 후킹합니다. 게임은 듀얼센스 햅틱 경로로 요청을 넘기기 *전에* 일반 모터용 프리셋을 이 함수로 넘기므로, 아무것도 바꾸지 않고 그 프리셋을 읽을 수 있습니다. 프리셋의 `cMotorVibration` 항목에는 지속 시간, 모터 종류, 세기, 페이드 플래그가 들어 있습니다. 모드는 이걸 자체 클럭으로 재생하고, 동시에 울리는 것들을 합성해서 현재 모터별 세기를 파일로 발행합니다.

**`plugin/plugin.cpp`** 는 게임 프로세스 안에서 그 값을 읽어, 듀얼센스 블루투스 출력 리포트 — 리포트 ID `0x31`, 78바이트, `0xA2` 시드의 CRC32 — 를 컨트롤러에 직접 씁니다.

플러그인이 필요한 이유는 REFramework의 lua 샌드박스가 HID 장치에 접근할 수 없고, 엔진 자체의 모터 경로(`via.hid.GamePadDevice.setMotorPower`)도 똑같이 조용한 햅틱 경로로 흘러가기 때문입니다. 리포트를 직접 쓰는 것 말고는 방법이 없습니다.

### 알아두면 좋은 것 두 가지

**Steam Input은 게임 프로세스에서 컨트롤러를 숨깁니다.** 장치를 여는 것 자체는 막지 않고, HID *조회*를 실패하게 만듭니다 — `HidD_GetAttributes`와 `HidD_GetPreparsedData`가 구조체는 제대로 채워놓고도 `FALSE`를 반환합니다. 그래서 플러그인은 `cfgmgr32`로 열거하고(Steam이 걸러내는 건 SetupAPI 쪽입니다), 인터페이스 경로로 패드를 식별하며, 필요 없는 기능 조회는 아예 요청하지 않고 리포트를 씁니다.

**Lua 파일 IO는 `reframework/data`로 샌드박스되어 있습니다.** lua 코드의 경로들이 파일명만 있는 이유가 이것입니다.

## 빌드

MinGW-w64 GCC 툴체인이 필요합니다. Visual Studio는 필요 없습니다 — 플러그인은 C ABI만 노출하고 경계를 넘나드는 C++ 객체가 없습니다.

```powershell
winget install BrechtSanders.WinLibs.POSIX.UCRT
.\build.ps1
```

`build.ps1`은 REFramework 플러그인 헤더를 받아오고, DLL을 컴파일하고, 두 언어의 릴리즈 압축 파일을 `out/`에 만듭니다. 영어판과 한글판은 lua의 `local LANGUAGE` 한 줄과 동봉된 README만 다릅니다.

`-static` 옵션이 중요합니다 — 이게 없으면 DLL 옆에 `libstdc++`, `libgcc`, `libwinpthread`가 같이 있어야 합니다. 지금 빌드는 `KERNEL32`, UCRT의 `api-ms-win-crt-*` 세트, `SETUPAPI`만 임포트합니다.

프로토콜 세부사항, 측정한 튜닝 수치, 남은 작업은 [NOTES.md](NOTES.md)에 있습니다.

## 설정

REFramework 메뉴(Insert 키)의 **Wilds DualSense BT Rumble** 항목에 있습니다.

맨 위에 있는 손잡이는 **진동 세기**로, 게임이 요청한 세기 대비 비율입니다. 기본값은 50%인데 듀얼센스에는 이 정도가 적당합니다 — 캡콤이 넣어둔 수치는 일반 패드의 편심 모터를 기준으로 맞춘 것이라, 보이스코일을 쓰는 듀얼센스에서는 같은 숫자가 훨씬 강하게 느껴지기 때문입니다.

**고급 설정** 아래에는:

| | |
|---|---|
| 강약 대비 (Gamma) | 약한 진동은 침묵 쪽으로, 타격은 또렷하게 |
| 약한 진동 차단 (Gate) | 이 값보다 약한 원본 진동은 아예 재생하지 않음 |
| LOW / HIGH | 저주파·고주파 모터를 각각 따로 |

또렷한 타격이 아니라 계속 웅웅거리는 느낌이라면 **약한 진동 차단(Gate)을 올리세요** — 모터가 도는 시간 자체를 줄이는 유일한 손잡이입니다. 세기를 내리는 건 오히려 역효과라 모든 진동이 좁은 구간에 뭉쳐버립니다. 실제로 전투 중 측정했을 때, Gate를 0.15에서 0.30으로 올리자 모터가 도는 시간이 38%에서 21%로 줄어드는 대신 실제 타격은 두 배로 강해졌습니다.

## 한계

- USB 케이블로 연결하면 모드는 스스로 잠듭니다. 그 상태에서는 게임의 진짜 햅틱이 동작하고, 그게 일반 진동보다 낫기 때문입니다.
- 적응형 트리거는 블루투스에서 여전히 동작하지 않습니다. 같은 USB 오디오 엔드포인트가 필요해서 여기서는 해결할 수 없습니다.
- 듀얼센스만 찾습니다. 다른 컨트롤러는 무시합니다.

## 크레딧

프로토콜 세부사항은 리눅스 [`hid-playstation`](https://github.com/torvalds/linux/blob/master/drivers/hid/hid-playstation.c) 드라이버를 참고했습니다. 플러그인 헤더는 [REFramework](https://github.com/praydog/REFramework)에서 가져왔습니다.

## 라이선스

[MIT](LICENSE)
