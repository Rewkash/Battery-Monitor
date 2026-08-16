#!/usr/bin/env python3
"""Generate a WiX source for the staged Windows application and build a per-user MSI."""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import re
import subprocess
import uuid
import xml.etree.ElementTree as ET


DEFAULT_IDENTITY_FILE = pathlib.Path(__file__).resolve().parents[2] / "CMakeLists.txt"
MARKER_COMPONENT_GUID = "59EEEA3B-8682-45A1-BE17-872452508E3E"
GUID_NAMESPACE = uuid.UUID("06eb77f5-4cb2-44d8-909f-7c2407bbb8ab")
WIX_NAMESPACE = "http://wixtoolset.org/schemas/v4/wxs"


def wix_id(prefix: str, value: str) -> str:
    return prefix + hashlib.sha256(value.encode("utf-8")).hexdigest()[:24]


def add_directory(parent: ET.Element, name: str, relative_path: str) -> ET.Element:
    return ET.SubElement(parent, "Directory", Id=wix_id("D", relative_path), Name=name)


def read_upgrade_code(identity_file: pathlib.Path) -> str:
    match = re.search(
        r'set\s*\(\s*BATTERY_MONITOR_MSI_UPGRADE_CODE\s+"\{([0-9A-Fa-f-]{36})\}"\s*\)',
        identity_file.read_text(encoding="utf-8"),
    )
    if match is None:
        raise SystemExit("BATTERY_MONITOR_MSI_UPGRADE_CODE is missing or invalid")
    return match.group(1).upper()


def read_cmake_string(identity_file: pathlib.Path, name: str, fallback: str) -> str:
    """Read a human-readable product define (e.g. the display name) from CMakeLists.txt."""
    match = re.search(
        r'set\s*\(\s*' + re.escape(name) + r'\s+"([^"]+)"\s*\)',
        identity_file.read_text(encoding="utf-8"),
    )
    return match.group(1) if match is not None else fallback


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--version", required=True)
    parser.add_argument("--wix", default="wix")
    parser.add_argument("--identity-file", default=DEFAULT_IDENTITY_FILE, type=pathlib.Path)
    args = parser.parse_args()

    source_root = args.input.resolve()
    upgrade_code = read_upgrade_code(args.identity_file)
    # Product naming (see README.md "Product naming"): the display name is
    # human-readable and sourced from CMake; the internal identity (upgrade
    # code, registry keys, folder paths, executable names) stays literal and
    # must never change, or upgrades and updates would break.
    display_name = read_cmake_string(args.identity_file, "BATTERY_MONITOR_DISPLAY_NAME", "ChargeView")
    publisher_name = read_cmake_string(args.identity_file, "BATTERY_MONITOR_PUBLISHER", "Orion Group")
    required = ("battery-monitor.exe", "battery-monitor-cli.exe", "battery-monitor-maintenance.exe")
    if not source_root.is_dir() or any(not (source_root / name).is_file() for name in required):
        raise SystemExit("staged application is incomplete")
    if len(args.version.split(".")) != 3 or not all(part.isdigit() for part in args.version.split(".")):
        raise SystemExit("MSI version must be numeric X.Y.Z")

    ET.register_namespace("", WIX_NAMESPACE)
    ET.register_namespace("ui", "http://wixtoolset.org/schemas/v4/wxs/ui")
    wix = ET.Element(f"{{{WIX_NAMESPACE}}}Wix")
    package = ET.SubElement(
        wix, "Package", Name=display_name, Manufacturer=publisher_name, Version=args.version,
        UpgradeCode=upgrade_code, Scope="perUser", Language="1049", Compressed="yes",
        ProductCode=str(uuid.uuid5(GUID_NAMESPACE, "product/" + args.version)).upper(),
    )
    ET.SubElement(package, "SummaryInformation", Description=f"{display_name} Battery Monitor",
                  Manufacturer=publisher_name)
    ET.SubElement(package, "MajorUpgrade",
                  DowngradeErrorMessage=f"A newer version of {display_name} is already installed.")
    ET.SubElement(package, "MediaTemplate", EmbedCab="yes")
    ET.SubElement(package, "Property", Id="WIXUI_EXITDIALOGOPTIONALCHECKBOXTEXT",
                  Value=f"Launch {display_name}")
    ET.SubElement(package, "Property", Id="WIXUI_EXITDIALOGOPTIONALCHECKBOX", Value="1")
    ET.SubElement(package, f"{{http://wixtoolset.org/schemas/v4/wxs/ui}}WixUI",
                  Id="WixUI_InstallDir", InstallDirectory="INSTALLFOLDER")

    standard = ET.SubElement(package, "StandardDirectory", Id="LocalAppDataFolder")
    programs = add_directory(standard, "Programs", "Programs")
    publisher = add_directory(programs, "Orion Group", "Programs/Orion Group")
    install = ET.SubElement(publisher, "Directory", Id="INSTALLFOLDER", Name="ChargeView")

    previous_install_folder = ET.SubElement(package, "Property", Id="PREVIOUSINSTALLFOLDER", Secure="yes")
    ET.SubElement(previous_install_folder, "RegistrySearch", Id="PreviousInstallFolderRegistry", Root="HKCU",
                  Key=r"Software\Orion Group\Battery Monitor\Install", Name="InstallLocation",
                  Type="raw", Bitness="always64")
    set_install_folder = ET.SubElement(package, "SetProperty", Id="INSTALLFOLDER",
                                       Value="[PREVIOUSINSTALLFOLDER]", After="AppSearch",
                                       Condition="PREVIOUSINSTALLFOLDER AND NOT Installed")
    ET.SubElement(package, "SetProperty", Id="ARPINSTALLLOCATION", Value="[INSTALLFOLDER]",
                  After="CostFinalize", Condition="NOT Installed")

    directories: dict[pathlib.PurePosixPath, ET.Element] = {pathlib.PurePosixPath(): install}
    component_ids: list[str] = []
    for file_path in sorted((path for path in source_root.rglob("*") if path.is_file()),
                            key=lambda path: path.relative_to(source_root).as_posix().casefold()):
        relative = pathlib.PurePosixPath(file_path.relative_to(source_root).as_posix())
        current = pathlib.PurePosixPath()
        parent = install
        for part in relative.parts[:-1]:
            current /= part
            if current not in directories:
                directories[current] = add_directory(parent, part, current.as_posix())
            parent = directories[current]
        component_id = wix_id("C", relative.as_posix())
        component_ids.append(component_id)
        component = ET.SubElement(
            parent, "Component", Id=component_id,
            Guid=str(uuid.uuid5(GUID_NAMESPACE, relative.as_posix())).upper(),
        )
        ET.SubElement(component, "File", Id=wix_id("F", relative.as_posix()),
                      Source=str(file_path))
        ET.SubElement(component, "RegistryValue", Root="HKCU",
                      Key=r"Software\Orion Group\Battery Monitor\Components",
                      Name=component_id, Type="integer", Value="1", KeyPath="yes")

    marker = ET.SubElement(install, "Component", Id="InstallerMarker", Guid=MARKER_COMPONENT_GUID)
    component_ids.append("InstallerMarker")
    registry_key = ET.SubElement(marker, "RegistryKey", Root="HKCU",
                                 Key=r"Software\Orion Group\Battery Monitor\Install")
    ET.SubElement(registry_key, "RegistryValue", Name="InstallMode", Type="string",
                  Value="msi-per-user", KeyPath="yes")
    ET.SubElement(registry_key, "RegistryValue", Name="InstallLocation", Type="string", Value="[INSTALLFOLDER]")
    ET.SubElement(registry_key, "RegistryValue", Name="ProductCode", Type="string", Value="[ProductCode]")
    ET.SubElement(registry_key, "RegistryValue", Name="UpgradeCode", Type="string", Value="{" + upgrade_code + "}")
    ET.SubElement(registry_key, "RegistryValue", Name="Version", Type="string", Value="[ProductVersion]")
    for relative_directory, directory_element in directories.items():
        ET.SubElement(marker, "RemoveFolder", Id=wix_id("R", relative_directory.as_posix() or "install"),
                      Directory=directory_element.attrib["Id"], On="uninstall")
    ET.SubElement(marker, "RemoveFolder", Id="RemovePublisherFolder", Directory=publisher.attrib["Id"],
                  On="uninstall")
    ET.SubElement(marker, "RemoveFolder", Id="RemoveProgramsFolder", Directory=programs.attrib["Id"],
                  On="uninstall")

    menu = ET.SubElement(package, "StandardDirectory", Id="ProgramMenuFolder")
    menu_dir = ET.SubElement(menu, "Directory", Id="ApplicationProgramsFolder", Name="ChargeView")
    shortcut_component = ET.SubElement(menu_dir, "Component", Id="StartMenuShortcut",
                                       Guid=str(uuid.uuid5(GUID_NAMESPACE, "start-menu")).upper())
    component_ids.append("StartMenuShortcut")
    ET.SubElement(shortcut_component, "Shortcut", Id="ApplicationStartMenuShortcut", Name=display_name,
                  Target="[INSTALLFOLDER]battery-monitor.exe", WorkingDirectory="INSTALLFOLDER")
    ET.SubElement(shortcut_component, "RemoveFolder", Id="RemoveApplicationProgramsFolder", On="uninstall")
    ET.SubElement(shortcut_component, "RegistryValue", Root="HKCU",
                  Key=r"Software\Orion Group\Battery Monitor\Install", Name="StartMenuShortcut",
                  Type="integer", Value="1", KeyPath="yes")

    desktop = ET.SubElement(package, "StandardDirectory", Id="DesktopFolder")
    desktop_component = ET.SubElement(desktop, "Component", Id="DesktopShortcut",
                                      Guid=str(uuid.uuid5(GUID_NAMESPACE, "desktop")).upper())
    component_ids.append("DesktopShortcut")
    ET.SubElement(desktop_component, "Shortcut", Id="ApplicationDesktopShortcut", Name=display_name,
                  Target="[INSTALLFOLDER]battery-monitor.exe", WorkingDirectory="INSTALLFOLDER")
    ET.SubElement(desktop_component, "RegistryValue", Root="HKCU",
                  Key=r"Software\Orion Group\Battery Monitor\Install", Name="DesktopShortcut",
                  Type="integer", Value="1", KeyPath="yes")

    ET.SubElement(package, "CustomAction", Id="LaunchChargeView", FileRef=wix_id("F", "battery-monitor.exe"),
                  ExeCommand="", Execute="immediate", Impersonate="yes", Return="asyncNoWait")
    ET.SubElement(package, "CustomAction", Id="AppendChargeViewToDriveRoot", Property="INSTALLFOLDER",
                  Value="[INSTALLFOLDER]ChargeView")
    ui = ET.SubElement(package, "UI")
    for letter in "ABCDEFGHIJKLMNOPQRSTUVWXYZ":
        ET.SubElement(ui, "Publish", Dialog="InstallDirDlg", Control="Next", Event="DoAction",
                      Value="AppendChargeViewToDriveRoot", Order="2",
                      Condition=f'INSTALLFOLDER = "{letter}:\\"')
    ET.SubElement(ui, "Publish", Dialog="InstallDirDlg", Control="Next", Event="SetTargetPath",
                  Value="INSTALLFOLDER", Order="3")
    ET.SubElement(ui, "Publish", Dialog="ExitDialog", Control="Finish", Event="DoAction",
                  Value="LaunchChargeView",
                  Condition="WIXUI_EXITDIALOGOPTIONALCHECKBOX = 1 AND NOT Installed")

    feature = ET.SubElement(package, "Feature", Id="MainFeature", Title=display_name, Level="1")
    for component_id in component_ids:
        ET.SubElement(feature, "ComponentRef", Id=component_id)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    source = args.output.with_suffix(".wxs")
    ET.ElementTree(wix).write(source, encoding="utf-8", xml_declaration=True)
    subprocess.run([args.wix, "build", "-arch", "x64", "-ext", "WixToolset.UI.wixext",
                    "-o", str(args.output), str(source)], check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
