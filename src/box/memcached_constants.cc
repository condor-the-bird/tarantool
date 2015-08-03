const char *memcached_binary_cmd_name[] = {
	/* 0x00 */ "GET",
	/* 0x01 */ "SET",
	/* 0x02 */ "ADD",
	/* 0x03 */ "REPLACE",
	/* 0x04 */ "DELETE",
	/* 0x05 */ "INCR",
	/* 0x06 */ "DECR",
	/* 0x07 */ "QUIT",
	/* 0x08 */ "FLUSH",
	/* 0x09 */ "GETQ",
	/* 0x0a */ "NOOP",
	/* 0x0b */ "VERSION",
	/* 0x0c */ "GETK",
	/* 0x0d */ "GETKQ",
	/* 0x0e */ "APPEND",
	/* 0x0f */ "PREPEND",
	/* 0x10 */ "STAT",
	/* 0x11 */ "SETQ",
	/* 0x12 */ "ADDQ",
	/* 0x13 */ "REPLACEQ",
	/* 0x14 */ "DELETEQ",
	/* 0x15 */ "INCRQ",
	/* 0x16 */ "DECRQ",
	/* 0x17 */ "QUITQ",
	/* 0x18 */ "FLUSHQ",
	/* 0x19 */ "APPENDQ",
	/* 0x1a */ "PREPENDQ",
	/* 0x1b */ "",
	/* 0x1c */ "TOUCH",
	/* 0x1d */ "GAT",
	/* 0x1e */ "GATQ",
	/* 0x1f */ "",
	/* 0x20 */ "SASL_LIST_MECHS",
	/* 0x21 */ "SASL_AUTH",
	/* 0x22 */ "SASL_STEP",
	/* 0x23 */ "GATK",
	/* 0x24 */ "GATKQ",
	/* 0x25 */ "",
	/* 0x26 */ "",
	/* 0x27 */ "",
	/* 0x28 */ "",
	/* 0x29 */ "",
	/* 0x2a */ "",
	/* 0x2b */ "",
	/* 0x2c */ "",
	/* 0x2d */ "",
	/* 0x2e */ "",
	/* 0x2f */ "",
	/* 0x30 */ "RGET",
	/* 0x31 */ "RSET",
	/* 0x32 */ "RSETQ",
	/* 0x33 */ "RAPPEND",
	/* 0x34 */ "RAPPENDQ",
	/* 0x35 */ "RPREPEND",
	/* 0x36 */ "RPREPENDQ",
	/* 0x37 */ "RDELETE",
	/* 0x38 */ "RDELETEQ",
	/* 0x39 */ "RINCR",
	/* 0x3a */ "RINCRQ",
	/* 0x3b */ "RDECR",
	/* 0x3c */ "RDECRQ"
};
