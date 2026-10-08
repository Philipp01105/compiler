# Binary cursors

Import "stdlib/binary". reader(&source) and writer(&mut destination) retain checked descriptor/backing-storage borrows. Reader supports independent Copy cursor positions; Writer retains exclusive access and moves. Neither defaults without a buffer.

readUint(width,endian)/writeUint(value,width,endian) accept widths 1,2,4,8 and Endian.Little/Big. Values must fit the selected width. readBytes(&mut destination)/writeBytes(&source) copy complete regions; storage must be disjoint. skip(count), offset() and remaining() manage bounded positions.

Operations are allocation-free and return core.Result with binary.Error. They preserve cursor position and output on failure; invalid width/value is checked before capacity. Stateless implementation cursors are private.
