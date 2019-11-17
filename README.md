# raw10p-to-grbg8

A simple utility to convert 10-bit packed Bayer into 8-bit GRBG format.

After the initial 10-bit packed is converted into 8-bit (by throwing out every 5th byte):
* BGGR is changed into GRBG by dropping the first and the last line of pixels (the output file height becomes less by 2)
* RGGB is changed into GRBG by dropping the first and the last raw of pixels (the output file width becomes less by 2)
* GBRG becomes GRBG after the first and the last lines and the fisrt and the last raws of pixels are removed (the output file height and width become less by 2)

The command line options are similar to the ones in raw2rgbpnm.
