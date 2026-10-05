# User Manual

## Main screen

![](_static/screenshot-main.png)

* **RSSI meter** (top left corner), also serves as a mono/stereo indicator in FM mode (one/two rows).
* **Settings save icon** (right after the RSSI meter). The settings are saved to non-volatile memory after 10 seconds of inactivity.
* **Bluetooth icon** (right after the save icon). Different colors indicate the connection status.
* **Wi-Fi icon** (top right area near the battery). Different colors indicate the connection status, and the icon blinks while connecting to a network.
* **Battery status** (top right corner). It doesn't show the voltage when charged, see [#36](https://github.com/esp32-si4732/ats-mini/issues/36#issuecomment-2778356143). The only indication that the battery is charging is the hardware LED on the bottom of the receiver, which turns ON during charging.
* **Band name and modulation** (VHF & FM, top center). See the [Bands table](#bands-table) for more details.
* **Info panel** (the box on the left side), also **Menu**. The parameters are explained in the [Menu](#menu) section. Also it can show the current time, or the date and time after a successful synchronization.
* **Frequency** (center of the screen). The active **VF**, **VM**, ETM, or ETM+ E-hour is right-aligned above the MHz/kHz unit. A red **M** indicates that the current frequency is saved in Memory, and a green **F** indicates that it is saved in Favorite. M, F, and tuning mode use fixed positions regardless of which values are visible, for example **M F VM**.
* **FM station name** (RDS PS) or **frequency name** (right below the frequency). A frequency name appears for some popular frequencies like FT8, SSTV, CB channels, or a shortwave [schedule](#schedule). Can also display current **menu option** using a bigger font when the Zoom Menu setting is enabled.
* **Tuning scale** (bottom of the screen). Can be replaced with additional RDS fields (RT, PTY) when extended RDS is enabled, or RSSI/SNR graphs in Scan mode.

## Alternative UI

![](_static/screenshot-smeter.png)

The differences are:

* **Stereo indicator** is on the right side of the band and mode (VHF & FM).
* **Tuning scale** (right under the station name). Numbers on the left & right sides are the band limits.
* **S/N Meter** (in dB). The range is 0...127 and the visual indicator linearly displays this range.
* **RSSI & S-Meter** (the number is in dBµV, the meter is in S-points). Please note that the RSSI range is also 0...127 (no negative values) and according to [these tables](https://dl4zao.de/_downloads/Dezibel.pdf) any values below S4 on HF (rssi < 4) and below S7 on VHF (rssi < 2) are bogus. Thus it is very far from being precise, and also depends on the antenna impedance.

Both meters can be replaced with additional RDS fields (RT, PTY) when extended RDS is enabled, or RSSI/SNR graphs in Scan mode.

## Controls

Controls are implemented through the encoder knob:

| Gesture                | Result                                                                |
|------------------------|-----------------------------------------------------------------------|
| Rotate                 | Tunes saved stations or steps frequency, navigates menu, adjusts parameters. |
| Click (<0.5 sec)       | Opens menu, selects.                                                  |
| Short press (0.5-2 sec) | Closes menus unless that screen uses the press for an action; opens Volume in VFO mode. |
| Long press (>2 sec)    | Sleep on/off.                                                         |
| Press and rotate       | Direct frequency input in VF mode, fine tuning in Seek mode.          |

### Direct frequency input mode

Direct frequency input is available only in **VF** mode.

* Press and rotate the encoder to select the step (digit or "half-digit").
* Rotate the encoder to adjust the frequency.
* Use short press to align frequency to the current step.
* To exit the mode, click the encoder or wait for a couple of seconds.

## Menu

The menu can be invoked by clicking the encoder button. Menus and adjustment panels remain open until they are closed with the encoder button or another action.

The three tuning-mode controls are at the top of the menu, followed by a separator. The active tuning mode is marked with `*` before its menu label.

* **V. Freq** - Switches directly to frequency-step tuning. VF is the only mode that allows direct frequency input by pressing and rotating.
* **V. Mem** - Switches directly to tuning frequencies saved in Memory. The selected VF or VM mode is saved across power cycles.
* **ETM** - Switches to ETM tuning. ETM uses the most recent ETM Scan list for FM and non-shortwave AM bands, and automatically switches to hourly ETM+ on AM shortwave bands. ETM+ uses the displayed local hour's list and automatically changes lists when the hour changes; the main-screen indicator shows E00 through E23. Changing bands while ETM is active switches between regular ETM and ETM+ automatically. A remote frequency command returns to VF.
* **Band** - List of [Bands](#bands-table).
* **Volume** - 0 (silent) ... 63 (max). The headphone volume level can be low (compared to the built-in speaker) due to limitation of the initial hardware design. Use short press to mute/unmute.
* **Seek** - Shown in the main menu only in VF mode. Seek up or down on AM/FM, normal tuning on LSB/USB (hardware seek function is not supported by SI4732 on SSB). Rotate or click the encoder to stop the seek. Use short press to switch between the seek and [schedule](#schedule) modes. Use press and rotate for manual fine tuning.
* **Scan** - Shown in the main menu only in VF mode. Scan a frequency range and plot the RSSI (S) and SNR (N) graphs (unfortunately, these metrics are almost meaningless in SSB modes due to SI4732 patch limitations). Both graphs are normalized to 0.0 - 1.0 range. While the Scan mode is active, short press the encoder for 0.5 seconds to rescan. To abort a running scan process click or rotate the encoder.
* **Memory** - Shown only in VF and VM modes. The current band's view starts with **Back**, a separator, **Add Current**, **ATS**, and **Clear**, followed by saved frequencies in ascending order. Back returns to the main menu. When the menu opens on a saved frequency, that frequency is selected automatically; otherwise it starts on Back. FM, MW, and SW each share one saved list across all bands of the same type, while browsing, deleting, clearing, and ATS operate only within the current band. Quickly clicking Add Current saves the currently tuned frequency immediately; a persistent message reports whether it was added, already saved, the list is full, or saving failed. For ATS or Clear, quickly click the action, select No or Yes, and quickly click again to confirm. ATS replaces the current band's frequencies with stations found in a whole-band scan while preserving saved frequencies for other bands in the same list. The left panel shows the count and five most recently added frequencies during scanning. Click or rotate to cancel a running scan and restore the Memory list from before the scan. The menu closes after Add Current or a scan completes successfully. Rotate the encoder to browse and tune saved frequencies. Press a saved frequency for at least 0.3 seconds to open No/Yes confirmation, then quickly click the selected answer. The lists are saved in flash and survive power off. Scanning uses the receiver's AM/FM seek function and is unavailable in LSB/USB modes; reception and antenna conditions affect which stations are found. Each shared list holds up to 256 stations; when full, existing frequencies are kept and new ones are skipped. Saved per-band lists from earlier firmware must be rescanned. With FM Region set to KR, saved FM stations identify the receiving area automatically. ATS or Clear returns any manually selected KR Area to Auto, so a subsequent scan uses automatic identification. Guide names are shown for that area and adjacent areas only. In Auto, if the scan cannot establish an area or distinguish broadcasters sharing a frequency, no guide name is shown. Received RDS names still take priority.
* **ETM Scan** - Shown only in ETM mode. Replaces the current band's ETM list with stations found in a whole-band scan. On AM shortwave bands it scans into one of 24 hourly ETM+ lists, **E00** through **E23**, selected from the displayed local time; each shortwave band and hour has its own list. On other AM/FM bands it uses one regular ETM list per band. Each list holds up to 256 frequencies and is saved in flash. Set the clock and UTC Offset before scanning an AM shortwave band. Click or rotate during scanning to cancel and preserve the previous list. ETM Scan is unavailable in LSB/USB modes. In VM and ETM, the station-name area shows the current frequency's position and total list size; a frequency outside the active list is shown as `- / total`.
* **Favorite** - 99 slots to store favorite frequencies. Short press (>0.5 sec) on an empty slot to store the current frequency, short press to erase a slot, switch between stored slots by rotating the encoder, click to exit the menu. A deleted slot can be restored with another short press while it remains selected. It is also possible to edit the favorite slots via [remote control](remote.md) or via the [web based tool](memory.md) in Google Chrome.
* **Settings** - Settings submenu. Its first **Back** item returns to the main menu and is followed by a separator. Another separator follows its last item. A separate main-menu separator follows Settings before the receiver controls below.

* **Squelch** - mute the speaker when the selected RSSI (dBuV) or SNR (dB) level is lower than the defined threshold. The setting is saved separately for each mode (FM, LSB, USB, AM). When Off, short press the encoder button to switch between RSSI and SNR. When enabled, short press turns squelch Off. Unlikely to work in SSB mode.
* **Bandwidth** - Selects the bandwidth of the channel filter.
* **AGC/ATTN** - Automatic Gain Control (on/off) or Attenuation level. The attenuator is not applicable to SSB mode.
* **AVC** - Sets the maximum gain for automatic volume control (not applicable to FM mode).
* **SoftMute** - Sets softmute max attenuation (only applicable to AM/SSB).
* **Mode** - FM (only available on the VHF band); LSB, USB, AM (available on other bands). The receiver doesn't support the NFM mode (on any band, including the CB) due to limitations of the SI4732 chip.
* **Step** - Tuning step (not every step is available on every band and mode).
* **NTP Now** - With Wi-Fi set to Off, close the menu, temporarily connect to a saved network, synchronize the clock, and turn Wi-Fi off again. The receiver remains usable while it connects and synchronizes. Press or rotate the encoder to cancel. The saved Wi-Fi mode does not change.

A separator follows NTP Now at the end of the main menu.

## Settings menu

* **Brightness** - Display brightness level (10...255). The minimal one draws about 80mA of the battery power, the default one about 100mA, the max level about 120mA.
* **Calibration** - SSB calibration offset (-2000...2000, per mode/band).
* **RDS** - Radio Data System options: PS - radio station name, CT - date and time, RT - text, PTY - genre, ALL (EU/US) - everything. RDS CT should contain UTC date and time, but some stations incorrectly transmit local or completely bogus values. The clock is synchronized from RDS only once. To synchronize it again, disable and re-enable RDS CT or switch the receiver off and on.
* **UTC Offset** - Affects the displayed date and time. Please note that automatic DST transitions are not supported; the offset needs to be adjusted manually.
* **Date/Time** - Set the UTC date and time with the encoder. Click to select the next field, or short press to set the clock and close the menu.
* **FM Region** - FM de-emphasis time constant by region (50µs for EU/JP/AU and KR, 75µs for the US). The default is KR. KR also enables offline Korean FM name lookup from a snapshot of the [Korean radio frequency list](https://namu.wiki/w/라디오%20주파수/대한민국). Planned, suspended, and closed stations are excluded. The list is bundled with the firmware; no Internet connection is needed on the radio. Names may be incomplete or outdated as broadcasters change frequencies.
* **KR Area** - When FM Region is KR, choose Auto or select one of 22 Korean receiving areas for FM station names. When another FM Region is active, selecting KR Area shows a persistent reminder to select KR first. A manual choice is saved across power cycles and also permits names from adjacent areas. Clearing the FM Memory list or using ATS resets the choice to Auto.
* **FM Stereo** - **Auto** lets the receiver blend down to mono on its own as the signal gets worse, **Mono** forces mono audio, trading the stereo image for less hiss on a weak station.
* **Theme** - Color theme.
* **UI Layout** - Alternative UI layouts. The default S-Meter layout has a large S-meter and S/N-meter.
* **Zoom Menu** - Display the currently selected menu item using a larger font (accessibility option).
* **Scroll Dir.** - Menu scroll direction for clockwise encoder turn.
* **Sleep** - Automatic sleep interval in seconds (0 - disabled).
* **Sleep Mode** - Locked - lock the encoder rotation during sleep; Unlocked - allow tuning the frequency in sleep mode; CPU Sleep - the maximum power saving mode. With the display being on, default brightness, and Wi-Fi the power consumption is about 170mA, without Wi-Fi 100mA, Locked/Unlocked modes draw about 70mA, CPU sleep mode draws about 40mA.
* **Load EiBi** - download the EiBi [schedule](#schedule) (requires Wi-Fi internet connection).
* **USB Port** - USB serial mode: Off (default) or Ad hoc. In Ad hoc mode, the receiver accepts the [remote control](remote.md) commands over the USB serial port.
* **TCP Port** - TCP control mode: Off (default) or Ad hoc. In Ad hoc mode, one client can use the [remote control](remote.md#tcp-over-wi-fi) commands over Wi-Fi on port 60000. Wi-Fi must be enabled separately.
* **Bluetooth** - Bluetooth LE mode: Off (default), Ad hoc, HID, or Unpair All. Ad hoc exposes the same [remote control](remote.md) protocol over BLE. HID makes the receiver act as a BLE HID central and connect to supported Bluetooth remotes/keyboards so their buttons can control tuning and menu actions. Unpair All clears all saved Bluetooth bonds and then switches Bluetooth to Off. WARNING: it is not recommended to enable both Bluetooth and Wi-Fi at the same time (the receiver might become unstable).
* **Wi-Fi** - Wi-Fi mode: Off (default), Access Point, Access Point + Connect, Connect, Sync Only. Connection and time synchronization run in the background, including at startup, so tuning and menus remain usable. More details on that below.
* **About** - Informational screens (Help, Authors, System).

## Wi-Fi

The Wi-Fi mode (2.4GHz only) can be used for the following purposes (for now):

* Time synchronization via NTP (Network Time Protocol) or the configuration web page.
* Download the EiBi shortwave schedule.
* Control the receiver over [TCP](remote.md#tcp-over-wi-fi).
* Viewing the receiver status (date/time and UTC offset, frequency, RSSI/SNR, volume, battery voltage, etc).
* Viewing the 99 Favorite slots and the shared FM/MW/SW Memory frequency lists.
* Downloading all Favorite and Memory entries as a YAML backup, or restoring them from that file.
* Manage the receiver settings.
* Upload or delete an optional [splash image](_static/splash-outdoor.png) shown when the receiver starts.

There are a couple of modes:

* **Off** (default)
* **AP Only** - Access Point mode. The receiver creates its own access point called `ATS-Mini` and starts the web server on <http://10.1.1.1>.
* **AP+Connect** - Access Point mode + try to connect to one of the three configured access points. If the connection succeeds, the receiver will synchronize the time every 5 minutes and start the web server on both <http://10.1.1.1> and a dynamic IP address it got from the configured access point.
* **Connect** - try to connect to one of the three configured access points, start the web server on a dynamic IP, then synchronize the time every 5 minutes.
* **Sync Only** - connect once to synchronize time, then turn Wi-Fi off. It runs at startup, but selecting it while the clock already has time keeps Wi-Fi disconnected. It does not reconnect after waking from CPU Sleep.

Initial configuration:

* Enable the **AP Only** mode (the receiver will briefly display its 10.1.1.1 IP address).
* Connect to the `ATS-Mini` access point from your phone or computer. There is no internet connection available on this access point. When connecting from a phone, it might be necessary to switch off the mobile data connection and any VPN/firewall software.
* Open a browser and visit the following URL: <http://10.1.1.1>. The status web page should open. Alternatively, you can try the mDNS address <atsmini.local> in your browser.
* Click the `Config` link. Here you can configure up to three access points the receiver will try to connect to, add optional login and password to protect the memory, settings, and firmware update pages, set the UTC date/time manually or from the browser, and change the UTC offset and other settings. Enable `Scan Hidden SSIDs` only if one of the configured access points does not broadcast its network name; leaving it off makes Wi-Fi connection faster. Leave `Half-step Encoder` off for the original EC11 20C20P encoder (20 clicks, 20 pulses), and enable it for an EC11 30C15P encoder (30 clicks, 15 pulses). Half-step mode is also required for the LILYGO T-Embed SI4732 hardware variant. See [discussion #87](https://github.com/esp32-si4732/ats-mini/discussions/87) for more information.
* After that, switch the Wi-Fi mode to **AP+Connect** or **Connect** (the receiver will briefly show its new dynamic IP address it got from a configured access point).
* Now connect your phone/computer to the same access point and open the new URL to check whether the receiver connected to the internet.

From now on you can switch the modes as you want and connect to your receiver either via the `ATS-Mini` internal access point (if enabled, mostly useful when there are no access points around), or via an external access point and a dynamic IP address. The receiver will always listen on the mDNS address <atsmini.local>.

```{hint}
When on the go, you can set up a mobile Wi-Fi hotspot on your smartphone and use it to connect the receiver to the internet.
```

<!-- ### Receiver settings available via Wi-Fi only -->

Firmware updates are available through the web interface's **Update** page. See [updating over Wi-Fi](flash.md#update-over-wi-fi) for instructions.

## Schedule

The receiver can download the [EiBi](http://eibispace.de/dx/eibi.txt) shortwave schedule and use it to display broadcasting stations, allowing you to quickly tune to them. Here’s how it works:

* The schedule only needs to be downloaded once via [Wi-Fi](#wi-fi). It will be stored in the receiver's flash memory so it doesn't need to be fetched every time the device powers on.
* To display scheduled stations correctly, the receiver’s clock must be set. You can set the UTC date/time from the Settings menu or the configuration web page, or configure a Wi-Fi internet connection and use Sync Only mode for NTP synchronization. The UTC offset setting doesn’t affect the schedule. A less reliable alternative is to use RDS CT, but this requires finding a station that broadcasts UTC time (not local time).
* Once set up, the receiver will display station names currently broadcasting on specific frequencies (only scheduled times are considered; days of the week are ignored for now).
* You can quickly jump between stations using the Seek mode (marked by a clock icon). To switch between modes, short press the encoder while in Seek mode.

## Reset

To reset the receiver settings (current band, frequency, favorite stations, downloaded schedule, etc):

1. Switch off the receiver
2. Press and hold the encoder
3. Turn on the receiver
4. Release the encoder after the `Resetting Preferences` message appears


## Bands table

| Name  | Min frequency | Max frequency | Default mode |
|-------|---------------|---------------|--------------|
| VHF  | 64 MHz        | 108 MHz       | FM           |
| MW1  | 150 kHz       | 1800 kHz      | AM           |
| MW2  | 495 kHz       | 1701 kHz      | AM           |
| MW3  | 1700 kHz      | 3500 kHz      | AM           |
| ALL  | 150 kHz       | 30000 kHz     | AM           |
| 11M  | 25600 kHz     | 26100 kHz     | AM           |
| 13M  | 21500 kHz     | 21900 kHz     | AM           |
| 15M  | 18900 kHz     | 19100 kHz     | AM           |
| 16M  | 17400 kHz     | 18100 kHz     | AM           |
| 19M  | 15100 kHz     | 15900 kHz     | AM           |
| 22M  | 13500 kHz     | 13900 kHz     | AM           |
| 25M  | 11000 kHz     | 13000 kHz     | AM           |
| 31M  | 9000 kHz      | 11000 kHz     | AM           |
| 41M  | 7000 kHz      | 9000 kHz      | AM           |
| 49M  | 5000 kHz      | 7000 kHz      | AM           |
| 60M  | 4000 kHz      | 5100 kHz      | AM           |
| 75M  | 3500 kHz      | 4000 kHz      | AM           |
| 90M  | 3000 kHz      | 3500 kHz      | AM           |
| 160M | 1800 kHz      | 2000 kHz      | LSB          |
| 80M  | 3500 kHz      | 4000 kHz      | LSB          |
| 40M  | 7000 kHz      | 7300 kHz      | LSB          |
| 30M  | 10000 kHz     | 10200 kHz     | LSB          |
| 20M  | 14000 kHz     | 14400 kHz     | USB          |
| 17M  | 18000 kHz     | 18200 kHz     | USB          |
| 15M  | 21000 kHz     | 21500 kHz     | USB          |
| 12M  | 24800 kHz     | 25000 kHz     | USB          |
| 10M  | 28000 kHz     | 29700 kHz     | USB          |
| CB   | 25000 kHz     | 28000 kHz     | AM           |
| SW   | 2300 kHz      | 26100 kHz     | AM           |

In the SW band, the current meter band is shown in small text beside the SW indicator, for example `SW 120m`. VF tuning, Memory ATS, and ETM Scan cover the ITU Region 3 shortwave broadcasting allocations from 11m through 120m. The complete 4750-5060 kHz 60m receiver band is included so 5000 kHz standard-frequency stations can also be found. Other gaps between allocations are skipped.

## Remote control

Various remote control options are documented on the dedicated [Remote control](remote.md) page.
