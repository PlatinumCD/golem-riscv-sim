/* Assembly spellings usable by the current LLVM and GNU assemblers.
 * Include with the C preprocessor in .S files. These emit one instruction. */
.macro net.send rd, destination, descriptor
    .insn r 0x2b, 0, 0, \rd, \destination, \descriptor
.endm
.macro net.recv rd
    .insn r 0x2b, 1, 0, \rd, zero, zero
.endm
.macro net.recv.try rd
    .insn r 0x2b, 1, 1, \rd, zero, zero
.endm
.macro net.info rd, token, field
    .insn r 0x2b, 2, 0, \rd, \token, \field
.endm
.macro net.release rd, token
    .insn r 0x2b, 3, 0, \rd, \token, zero
.endm
.macro net.wait rd, ticket
    .insn r 0x2b, 4, 0, \rd, \ticket, zero
.endm
.macro net.wait.try rd, ticket
    .insn r 0x2b, 4, 1, \rd, \ticket, zero
.endm
