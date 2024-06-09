# liblzx

LZX compression algorithm format C/C++ library

### Status

- [x] improved Y2K date fixes based on [UnLZX 2.2 source][3]
- [x] introduce -p [TITOV|CALUSINSKI] getopt optarg
- [x] use C time.h for date/time
- [x] Makefile support

### Build

```sh
make install
```

### Run

```sh
unlzx -p TITOV -v samples/Test_LZX.lzx
```

### References

- [LZX v1.21r - The most powerful archiver available for the Amiga][1]
- [UnLZX 2.2 Total Commander Packer Plugin website][2]
- [UnLZX 2.2 Total Commander Packer Plugin source][3]
- [LzxPack 1/2][4]
- [Convert LIT 1.8][5]

[1]: http://xavprods.free.fr/lzx/ "LZX v1.21r"
[2]: https://totalcmd.net/plugring/UnLZX.html "UnLZX 2.2 Total Commander Packer Plugin website"
[3]: https://ghisler.fileburst.com/plugins/lzx_source.zip "UnLZX 2.2 source"
[4]: https://busy.speccy.cz/tvorba/pcprogs.htm "LzxPack 1/2"
[5]: http://www.convertlit.com/download.php "Convert LIT 1.8"
