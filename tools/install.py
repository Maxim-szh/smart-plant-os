#!/usr/bin/env python3
"""Build, provision and flash a fresh Smart Plant OS Hub + one Node installation."""
import argparse
from getpass import getpass
import importlib.util
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
from datetime import datetime

ROOT = Path(__file__).resolve().parents[1]
IDF = Path(os.environ.get("IDF_PATH", ""))
NVS_GENERATOR = IDF / "components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py"


def run(args, *, capture=False):
    print("+", " ".join(map(str, args)))
    return subprocess.run(list(map(str, args)), cwd=ROOT, check=True,
                          text=True, capture_output=capture)


def get_mac(chip, port):
    result = run([sys.executable, "-m", "esptool", "--chip", chip,
                  "--port", port, "read_mac"], capture=True)
    match = re.search(r"^MAC:\s*([0-9a-fA-F:]{17})\s*$", result.stdout, re.M)
    if not match:
        raise RuntimeError(f"Не удалось прочитать MAC платы {chip} на {port}. Проверьте USB-порт и BOOT.")
    return match.group(1).lower()


def build(project, target, profile=None):
    project_path = ROOT / "firmware" / project
    name = f"install-{profile or target}"
    build_path = project_path / "build" / name
    config = build_path / "sdkconfig"
    build_path.parent.mkdir(parents=True, exist_ok=True)
    # A previous interrupted first configure may leave a non-CMake directory here.
    if build_path.exists() and not (build_path / "CMakeCache.txt").exists():
        shutil.rmtree(build_path)
    defaults = (project_path / "sdkconfig.defaults").read_text()
    if profile == "n8r2":
        defaults = defaults.replace("CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y", "CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y")
        defaults = defaults.replace("CONFIG_SPIRAM_MODE_OCT=y", "CONFIG_SPIRAM_MODE_QUAD=y")
    profile_defaults = build_path.parent / f"{name}.sdkconfig.defaults"
    profile_defaults.write_text(defaults)
    args = ["idf.py", "-C", project_path, "-B", build_path,
            "-D", f"SDKCONFIG={config}", "-D", f"SDKCONFIG_DEFAULTS={profile_defaults}"]
    run([*args, "set-target", target])
    run([*args, "build"])
    return args, build_path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--hub-port", required=True, help="USB port for ESP32-S3 Hub")
    parser.add_argument("--node-port", action="append", required=True,
                        help="USB port for an ESP32-C3 Node; repeat for more Nodes (up to 10)")
    parser.add_argument("--hub-profile", choices=("n16r8", "n8r2"), required=True,
                        help="S3 flash/PSRAM profile matching the board marking")
    parser.add_argument("--channel", type=int, default=6, help="shared Wi-Fi/ESP-NOW channel (default: 6)")
    parser.add_argument("--country", default="01", help="ESP-IDF 5.3.4 country code (default: 01)")
    parser.add_argument("--wifi-ssid", help="home 2.4 GHz Wi-Fi name; router channel must match --channel")
    parser.add_argument("--wifi-password", help="avoid this option because it may be stored in shell history")
    args = parser.parse_args()
    if not os.environ.get("IDF_PATH") or not (IDF / "export.sh").exists():
        parser.error("ESP-IDF 5.3.4 is required. Open its ESP-IDF terminal, then run this command again.")
    if not NVS_GENERATOR.exists():
        parser.error(f"ESP-IDF NVS generator not found: {NVS_GENERATOR}")
    if not shutil.which("idf.py") or not shutil.which("openssl"):
        parser.error("Activate ESP-IDF 5.3.4 environment and install OpenSSL before running the installer.")
    version = run(["idf.py", "--version"], capture=True).stdout
    if "5.3.4" not in version:
        parser.error(f"This installer requires ESP-IDF 5.3.4; found: {version.strip()}")
    if not 1 <= len(args.node_port) <= 10:
        parser.error("Connect one to ten Nodes")
    if args.hub_port in args.node_port or len(set(args.node_port)) != len(args.node_port):
        parser.error("Every board must use a different USB port")
    wifi_ssid = args.wifi_ssid
    wifi_password = args.wifi_password or ""
    if wifi_ssid is None:
        wifi_ssid = input("Домашний Wi-Fi 2,4 ГГц (Enter — пока только точка SmartPlantOS): ").strip()
    if wifi_ssid and not wifi_password:
        wifi_password = getpass("Пароль домашнего Wi-Fi (не отображается): ")
    if len(wifi_ssid.encode("utf-8")) > 32:
        parser.error("Домашнее имя Wi-Fi должно занимать не более 32 байт")
    password_bytes = wifi_password.encode("utf-8")
    if wifi_ssid and (not 8 <= len(password_bytes) <= 63 or any(byte < 0x20 or byte > 0x7e for byte in password_bytes)):
        parser.error("Пароль Wi-Fi должен содержать 8–63 печатных ASCII-символа")
    if not wifi_ssid and wifi_password:
        parser.error("Нельзя задать пароль домашней сети без имени сети")
    print("ВНИМАНИЕ: это первичная установка с новыми ключами и новой конфигурацией.")
    print("Не запускайте мастер для обновления уже работающей установки.")
    if input("Для продолжения введите INSTALL: ").strip() != "INSTALL":
        print("Отменено; платы не изменены.")
        return

    node_macs = [get_mac("esp32c3", port) for port in args.node_port]
    hub_mac = get_mac("esp32s3", args.hub_port)
    print(f"Detected {len(node_macs)} C3 Node(s) and S3 Hub STA MAC {hub_mac}.")
    print("Actuator GPIOs remain disabled; no pump can be started by this installation.")
    node_build, _ = build("node", "esp32c3")
    hub_build, _ = build("hub", "esp32s3", args.hub_profile)
    for port in args.node_port:
        run([*node_build, "-p", port, "flash"])
    run([*hub_build, "-p", args.hub_port, "flash"])

    spec = importlib.util.spec_from_file_location("provision", ROOT / "tools/provision.py")
    provision = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(provision)
    output = ROOT / "secrets" / f"install-{datetime.now().strftime('%Y%m%d-%H%M%S')}"
    namespace = argparse.Namespace(
        hub_mac=provision.mac(hub_mac), node_mac=[provision.mac(mac) for mac in node_macs], pins=None,
        country=args.country, channel=args.channel, wifi_ssid=wifi_ssid, wifi_password=wifi_password,
        output=output, web=True)
    provision.generate(namespace)
    names = ["hub", *(f"node-{i}" for i in range(len(node_macs)))]
    for name in names:
        run([sys.executable, NVS_GENERATOR, "generate", output / f"{name}.csv",
             output / f"{name}.nvs.bin", "0x20000"])
    run([sys.executable, "-m", "esptool", "--chip", "esp32s3", "--port", args.hub_port,
         "write_flash", "0x9000", output / "hub.nvs.bin"])
    for port, i in zip(args.node_port, range(len(node_macs))):
        run([sys.executable, "-m", "esptool", "--chip", "esp32c3", "--port", port,
             "write_flash", "0x9000", output / f"node-{i}.nvs.bin"])
    print(f"\nУстановка готова: один Hub и {len(node_macs)} Node.")
    print(f"Первый вход: Wi-Fi SmartPlantOS → https://192.168.4.1; пароль и ключ входа: {output / 'access.json'}")
    if wifi_ssid:
        print(f"Hub также подключается к домашней сети {wifi_ssid!r}; IP-адрес появится в serial monitor/списке клиентов роутера.")
        print(f"Важно: канал роутера 2,4 ГГц должен быть закреплён на канале {args.channel}, иначе ESP-NOW Nodes не свяжутся с Hub.")
    print("Сохраните каталог secrets: повторная генерация ключей не эквивалентна восстановлению установки.")


if __name__ == "__main__":
    try:
        main()
    except (OSError, subprocess.CalledProcessError, RuntimeError, ValueError) as error:
        print(f"Установка остановлена: {error}", file=sys.stderr)
        sys.exit(1)
