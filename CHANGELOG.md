# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/zh-TW/1.1.0/) and this project adheres to [Semantic Versioning](https://semver.org/).

## [1.3.16] - 2025-12-08

### Changed
- Support Vulcan sensor.
- Improve memory allocation safety for guest data.

## [1.3.15] - 2024-11-13

### Changed
- Improved driver name recognition stability.
- Removed redundant register reads on touchpad devices.
- Reset sensor to renew the partition information to improve the performance.


## [1.3.14] - 2023-08-18

### Added
- Added ARM 32-bit .deb package
### Changed
- Adjusted command timeouts for ARM 32-bit.
- Set the default device type to touchpad to prevent problems after device reset.
- Added argument to select device by PID.


## [1.3.13] - 2023-07-06

### Added
- Added GitHub workflow to build binaries.
### Changed
- Disabled sensor interrupts during updates for better stability.
