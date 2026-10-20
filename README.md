# ios-ginkgo

**iOS 16 rodando no Redmi Note 8 (ginkgo).**

Este branch é um port do [MakrSas/ios-topaz](https://github.com/MakrSas/ios-topaz) (Redmi Note 12 4G), que por sua vez vem do [dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet) (Redmi Note 10 Pro), para o **Xiaomi Redmi Note 8** (`ginkgo`, Qualcomm **SM6125 "trinket"** / Snapdragon 665, versão de 4 GB).

Por cima do kernel XNU de código aberto da Apple (xnu-8796) roda o **sistema do iPhone X, iOS 16.7.16 (20H392), sem modificações**.

> Projeto experimental. Não é um sistema utilizável no dia a dia. A instalação usa a partição `userdata` e **apaga os dados do Android**. Nada é gravado nas partições de boot: o kernel é iniciado da RAM com `fastboot boot`.

## O que funciona

| Área | Situação |
|---|---|
| Boot | ABL da Xiaomi (desbloqueado) → `fastboot boot` do shim (boot image **v2**) → XNU |
| CPU | Troca do núcleo Silver para o Gold `0x100` (os Silver não suportam páginas de 16K). Testado com `cpus=1`. Gold a 2016 MHz |
| Memória | 3 GB em `0x80000000` (único bloco grande de RAM livre na versão de 4 GB) |
| Armazenamento | **Driver novo de eMMC** (SDHCI 3.0, `sdhci@4744000`): reinicializa o cartão, barramento de 8 bits em high speed, leitura e gravação por **ADMA2**. A raiz do iOS é um volume HFS+ na `userdata` |
| Tela | Framebuffer do ABL em `0x5c000000` (1080×2340), desenho por software. Apresentado ao iOS como a tela do iPhone X |
| Interface | Maçã de boot, tela de bloqueio, Siri |
| Botões | Power (segurar abre a Siri), Volume + e Volume − (o indicador de volume aparece) |
| Toque | Barramento SPI funcionando e chip FT8719 respondendo o ID (`87 19`), mas **sem leitura de toque** (veja abaixo) |

## O que não funciona

- **Toque.** O FT8719 da Huaxing é integrado à tela e fica no modo de inicialização até receber o firmware de toque. No Android, é o driver da Xiaomi que faz isso. Este port não faz.
- **Wi-Fi, Bluetooth, rede celular, áudio, câmera, sensores e GPU.**
- **Troca de buffer da tela.** A troca de scanout do MDP não tem efeito no ginkgo, então é preciso usar `sweetfb_noflip=1`. Com isso, o console e a interface compartilham o mesmo buffer.
- **Relógio.** Sem RTC, a data começa em 31 de dezembro.
- **Brilho.** A tela fica escura.

## Diferenças em relação ao topaz

| Item | topaz (Note 12 4G) | ginkgo (Note 8) |
|---|---|---|
| SoC | SM6225 "khaje" | SM6125 "trinket" |
| Núcleos Gold | `0x100`–`0x103` | `0x100`–`0x103` (igual) |
| GIC | `0xf200000` | `0xf200000` (igual) |
| Framebuffer | `0x5c000000`, 1080×2400 | `0x5c000000`, **1080×2340** |
| Formato do boot image | v4 | **v2** |
| RAM do kernel | `0x1_40000000` + 4 GB | **`0x80000000` + 3 GB** |
| Armazenamento | UFS (`0x4804000`) | **eMMC (`0x4744000`)** |
| Toque | FT5452, I²C em QUP0 SE2 | **FT8719, SPI** em QUP0 SE2, IRQ GPIO88, reset GPIO87 |
| Botões | PM6125 PON + GPIO5 | igual |
| Tela | 1080×2400 DSC | FT8719 Huaxing, video mode |

## Arquivos deste branch

| Caminho | O que é |
|---|---|
| `shim/build-ginkgo.sh` | Gera `shim-ginkgo.img` (boot image v2 com o DTB original). Sem argumento, só o teste do shim; com `payload.bin`, inicia o XNU |
| `shim/sdhci.c` | Teste de eMMC no shim (PIO e ADMA2), usado na adaptação |
| `shim/console.h`, `shim/shim.c` | `-DGINKGO`: tela de 2340 linhas, sem escritas de UFS/IMEM/PS_HOLD do topaz |
| `tools/mkadt.py --ginkgo` | Device tree da Apple para o Note 8 (RAM, tela, nó `sweet-sdhci`) |
| `xnu/patches/0200-ginkgo-emmc.patch` | Backend eMMC/SDHCI no driver de disco (`ufsroot0`) |
| `xnu/patches/0201-ginkgo-fb-noflip.patch` | `sweetfb_noflip=1`: mantém o scanout no framebuffer do boot |
| `xnu/patches/0202`–`0205` | Driver SPI do toque FT8719 e diagnósticos |
| `xnu/patches/0206-ginkgo-ft8719-pram-download.patch` | Carrega o firmware do toque na RAM do FT8719 (boot ROM, `0x55`/`0xAE`/ECC `0xCC`/`0x08`), só se o payload trouxer a imagem |

## Como montar

Os passos gerais (compilar o kernel no GitHub Actions, montar a imagem do iOS e gravar na `userdata`) são os mesmos do [README do ios-topaz](https://github.com/MakrSas/ios-topaz/blob/topaz/README.md). O que muda no ginkgo:

**Backup primeiro.** Guarde `persist`, `modemst1`, `modemst2`, `fsg`, `fsc`, `boot`, `dtbo` e `vbmeta`. O shim usa o DTB do seu `boot.img` original:
```
python3 tools/unpack_bootv2.py backup/boot.img dtb/boot_unpacked
```

**Kernel:** rode o workflow `xnu.yml` no branch `ginkgo`.

**Boot image:**
```
python3 tools/mkadt.py shim/out/ginkgo.adt --ginkgo
python3 tools/mkpayload.py xnu/out/kernel.development.sweet shim/out/ginkgo.adt shim/out/payload.bin \
  --cmdline 'debug=0xc serial=3 cpus=1 keepsyms=1 -noprogress rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin ufsrw=1 rootrw=1 launchd_unsecure_cache=1 wdt=-1 sweetfb_hz=30 sweettouch=0 sweetbattery=0 sweetfb_noflip=1 sweetfb_noconsole=1 sweetfb_stayon=1'
cd shim && ./build-ginkgo.sh ../shim/out/payload.bin
```

**Gravar e iniciar:**
```
fastboot -S 512M flash userdata ios16-root.simg
fastboot boot shim/out/shim-ginkgo.img
```

### Boot-args específicos do ginkgo

| Boot-arg | Efeito |
|---|---|
| `sweetfb_noflip=1` | Obrigatório: não troca o scanout (a troca não funciona no ginkgo) |
| `sweetfb_stayon=1` | Ignora o apagar de tela do bloqueio automático |
| `emmcdiv=N` | Divisor de clock do eMMC (padrão 4, no máximo ~50 MHz) |
| `sweettouch=0` | Desliga o driver de toque |
| `sweettouchdelay=N` | Espera N segundos antes de iniciar o toque |
| `sweettouchdiag=1` | Congela a tela com um resumo se o driver de toque parar |
| `sweettouchfw=0` | Não carrega o firmware do toque mesmo que o payload traga a imagem |

### Firmware do toque (teste)

O FT8719 fica no boot ROM até o host carregar o firmware na RAM dele (não grava a flash). O patch 0206 faz isso a partir de uma imagem passada ao `mkpayload.py`:
```
python3 tools/mkpayload.py xnu/out/kernel.development.sweet shim/out/ginkgo.adt shim/out/payload.bin \
  --touchfw /caminho/fora/do/repo/fw_huaxing_v09.bin --cmdline '...'
```
A imagem vai só para o `payload.bin`/boot image local. **Não versione o firmware nem o publique em artefatos**: o arquivo não tem licença explícita. Sem `--touchfw` o comportamento é o de antes.

### Observações

- **WSL:** o kernel padrão do WSL2 não tem HFS+. Para montar a imagem, compile o kernel do WSL (`linux-msft-wsl-*`) com `CONFIG_HFSPLUS_FS=y` e aponte o `.wslconfig` para ele.
- **Não versione** `*.img`, `*.simg`, `research/` nem backups: são arquivos grandes, com dados do aparelho ou da Apple.

## Voltar ao Android

TWRP ou OrangeFox → *Wipe* → *Format Data*. Os backups de `persist` e `modemst*` podem ser restaurados com `fastboot flash`.

## Créditos

Baseado no [ios-topaz](https://github.com/MakrSas/ios-topaz) e no [ios-sweet](https://github.com/dipa-studio/ios-sweet), que fizeram a maior parte do trabalho. Usa os fontes abertos da Apple (xnu, IOHIDFamily, libpthread, hfs), [BearSSL](https://bearssl.org), [blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build), [ipsw](https://github.com/blacktop/ipsw) e o kernel publicado pela Xiaomi para o ginkgo (referência de hardware). O iOS pertence à Apple e não é distribuído aqui.
