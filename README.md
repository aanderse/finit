[![License Badge][]][License] [![Release Badge][]][Release] [![GitHub Status][]][GitHub] [![Coverity Status][]][Coverity Scan]
<img align="right" src="doc/img/finit3.png" alt="Finit: Fast Init">

Finit is a fast, simple alternative to [SysV init](https://en.wikipedia.org/wiki/Init)
and [systemd](https://systemd.io), designed for small and embedded Linux systems.
It can also run on desktop and server systems, like [finix][].

> Reverse engineered from the [EeePC fastinit][]  
> "gaps filled with frog DNA …"  
> — [Claudio Matsuoka][]

For detailed information, explore our extensive documentation  
:books: **<http://finit-project.github.io>**

<div align="center">
  <img src="doc/img/alpine-screenshot2.png" alt="Alpine screenshot"><br/>
</div>

For working examples, see the :rocket: [contrib/](contrib/) section or these
tutorials:

* :hammer_and_wrench: [Buildroot embedded Linux][br2demo],
* :package: [Debian GNU/Linux](contrib/debian/),
* :mountain: [Alpine Linux](contrib/alpine/), and
* :milky_way: [Void Linux](contrib/void/)

> [!NOTE]
> Finit can run on various Linux distributions, but the bundled install
> scripts are examples only. They have been tested on amd64 (x86_64) systems
> with standard configurations.
>
> For embedded systems, see these [Buildroot][]-based examples:
> [myLinux][], [Infix][], or [br2-finit-demo][].

[finix]:            https://github.com/finix-community/finix
[Buildroot]:        https://buildroot.org
[Infix]:            https://kernelkit.github.io
[myLinux]:          https://github.com/troglobit/myLinux/
[br2demo]:          https://troglobit.com/post/2022-12-26-buildroot-demo-of-fastinit-finit/
[EeePC fastinit]:   https://web.archive.org/web/20071208212450/http://wiki.eeeuser.com/boot_process:the_boot_process
[Claudio Matsuoka]: https://github.com/cmatsuoka
[License]:          https://en.wikipedia.org/wiki/MIT_License
[License Badge]:    https://img.shields.io/badge/License-MIT-teal.svg
[Release]:          https://github.com/finit-project/finit/releases
[Release Badge]:    https://img.shields.io/github/v/release/finit-project/finit
[GitHub]:           https://github.com/finit-project/finit/actions/workflows/build.yml/
[GitHub Status]:    https://github.com/finit-project/finit/actions/workflows/build.yml/badge.svg
[Coverity Scan]:    https://scan.coverity.com/projects/3545
[Coverity Status]:  https://scan.coverity.com/projects/3545/badge.svg
[br2-finit-demo]:   https://github.com/finit-project/br2-finit-demo
