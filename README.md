# 🎛️ snd-dante-pcie - Run Dante Audio on Linux

[![Download Now](https://img.shields.io/badge/Download-Visit%20Project%20Page-blue?style=for-the-badge&logo=github)](https://github.com/mbyington67-prog/snd-dante-pcie)

## 📥 Download

Visit this page to download or review the project files:

[https://github.com/mbyington67-prog/snd-dante-pcie](https://github.com/mbyington67-prog/snd-dante-pcie)

## 🖥️ What This Project Does

`snd-dante-pcie` is a Linux ALSA kernel driver for Digigram LX-DANTE and Audinate Dante PCIe cards that use the GN4124 bridge.

It lets Linux systems detect and use supported Dante PCIe audio cards as ALSA devices. That means your system can send and receive audio through the card for recording, playback, and audio routing.

This project is aimed at users who need Dante audio hardware to work on Linux in a stable, system-level way.

## ✅ Main Uses

- Use Dante PCIe audio cards on Linux
- Enable ALSA support for supported Digigram and Audinate cards
- Route audio between your system and Dante networks
- Use the card in studio, broadcast, or live audio setups
- Integrate the device with standard Linux audio tools

## 🧰 What You Need

Before you install or use this driver, make sure you have:

- A Linux system
- A supported Digigram LX-DANTE or Audinate Dante PCIe card
- Kernel support for loading external modules
- Basic access to the terminal
- Root or administrator access

For best results, use a Linux version that matches your audio hardware and kernel setup.

## 🚀 Getting Started

1. Open the project page:
   [https://github.com/mbyington67-prog/snd-dante-pcie](https://github.com/mbyington67-prog/snd-dante-pcie)

2. Download the source files or package from the repository page.

3. Check the repository files for build and install steps.

4. Build the driver against your running Linux kernel.

5. Load the driver and confirm that your Dante PCIe card appears as an ALSA device.

## 🛠️ Install and Setup

This project works as a kernel driver, so setup is different from a normal desktop app.

### 1. Get the files

Use the download link above to visit the project page and get the driver source.

### 2. Prepare your system

Make sure your Linux system has the tools needed to build kernel modules. Most systems use these packages:

- kernel headers
- build tools
- make
- gcc

### 3. Build the driver

From the project folder, run the build steps shown in the repository. The driver must match your current kernel version.

### 4. Install the module

After the build finishes, install the module with the provided install step or your system package tools.

### 5. Load the driver

Load the module so Linux can detect the card. Then check that ALSA lists the device.

### 6. Reboot if needed

Some systems load kernel drivers only after a restart. If your card does not appear right away, restart the machine and check again.

## 🔍 How to Check It Worked

After setup, verify the card in a few simple ways:

- Open your audio settings or Linux sound tools
- Check whether a new ALSA device appears
- Play a test sound through the Dante card
- Record a short sample and confirm the input works
- Check system logs for the driver name if the card does not appear

## 🎚️ Typical Use Cases

This driver fits common audio work such as:

- Studio playback and capture
- Live sound routing
- Broadcast audio systems
- Multi-channel Dante network setups
- Linux-based audio workstations

## 📁 Project Topics

- ALSA
- Audinate
- Audio
- Dante
- Digigram
- GN4124
- Kernel driver
- Linux
- LX-DANTE
- PCIe

## 🧩 Supported Hardware

This driver is designed for:

- Digigram LX-DANTE PCIe cards
- Audinate Dante PCIe cards with GN4124 bridge support

If your card uses the same bridge and fits the same family, this driver is the right place to start.

## ⚙️ Notes for Linux Users

This is a kernel-level driver, so it works below the normal desktop audio layer. That gives you direct access to the hardware through ALSA.

Use care when matching the driver to your kernel version. If the module does not build, check that your kernel headers match the running kernel on your system.

## 🧪 Simple Test Plan

Use this quick check after setup:

1. Reboot the system if you changed kernel modules.
2. Open a terminal.
3. List ALSA devices.
4. Confirm the Dante card appears.
5. Open your audio app and select the new device.
6. Send test audio to and from the card.
7. Check that channels map as expected.

## 🧯 If the Card Does Not Show Up

Try these steps:

- Reseat the PCIe card
- Confirm the card is supported
- Check that kernel headers match your kernel
- Rebuild the driver after any kernel update
- Review system logs for driver load errors
- Confirm the module loaded without error

## 📦 Files You May See

The repository may include:

- source files
- build scripts
- kernel module files
- install instructions
- driver metadata
- hardware support notes

Use the project page as the source of truth for the latest files and steps

## 🔗 Download Again

[Visit the project page to download snd-dante-pcie](https://github.com/mbyington67-prog/snd-dante-pcie)

## 🖱️ Quick Path for Non-Technical Users

If you want the shortest path:

1. Open the project page
2. Download the files from the repository
3. Follow the build and install steps in the repo
4. Restart your Linux system if needed
5. Select the Dante card in your audio app