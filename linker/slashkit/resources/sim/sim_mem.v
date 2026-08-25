/**
 * The MIT License (MIT)
 * Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software
 * and associated documentation files (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge, publish, distribute,
 * sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
 * NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
 
`timescale 1ns / 1ps

module sim_mem
#(
    parameter MEM_WIDTH = 64,
    parameter MEM_DEPTH_LOG = 34,
    parameter READ_LATENCY = 50
)(

(* X_INTERFACE_PARAMETER = "MODE Slave, MASTER_TYPE BRAM_CTRL, MEM_ECC NONE, READ_WRITE_MODE READ_WRITE" *)
(* X_INTERFACE_INFO = "xilinx.com:interface:bram_rtl:1.0 MEM_PORT_A CLK" *)
    input clk_a,
(* X_INTERFACE_INFO = "xilinx.com:interface:bram_rtl:1.0 MEM_PORT_A RST" *)
    input rst_a,
(* X_INTERFACE_INFO = "xilinx.com:interface:bram_rtl:1.0 MEM_PORT_A EN" *)
    input en_a,
(* X_INTERFACE_INFO = "xilinx.com:interface:bram_rtl:1.0 MEM_PORT_A ADDR" *)
    input [MEM_DEPTH_LOG-1:0] addr_a,
(* X_INTERFACE_INFO = "xilinx.com:interface:bram_rtl:1.0 MEM_PORT_A WE" *)
    input [MEM_WIDTH/8-1:0] we_a,
(* X_INTERFACE_INFO = "xilinx.com:interface:bram_rtl:1.0 MEM_PORT_A DIN" *)
    input [MEM_WIDTH-1:0] din_a,
(* X_INTERFACE_INFO = "xilinx.com:interface:bram_rtl:1.0 MEM_PORT_A DOUT" *)
    output [MEM_WIDTH-1:0] dout_a,

(* X_INTERFACE_PARAMETER = "MODE Slave, MASTER_TYPE BRAM_CTRL, MEM_ECC NONE, READ_WRITE_MODE READ_WRITE" *)
(* X_INTERFACE_INFO = "xilinx.com:interface:bram_rtl:1.0 MEM_PORT_B CLK" *)
    input clk_b,
(* X_INTERFACE_INFO = "xilinx.com:interface:bram_rtl:1.0 MEM_PORT_B RST" *)
    input rst_b,
(* X_INTERFACE_INFO = "xilinx.com:interface:bram_rtl:1.0 MEM_PORT_B EN" *)
    input en_b,
(* X_INTERFACE_INFO = "xilinx.com:interface:bram_rtl:1.0 MEM_PORT_B ADDR" *)
    input [MEM_DEPTH_LOG-1:0] addr_b,
(* X_INTERFACE_INFO = "xilinx.com:interface:bram_rtl:1.0 MEM_PORT_B WE" *)
    input [MEM_WIDTH/8-1:0] we_b,
(* X_INTERFACE_INFO = "xilinx.com:interface:bram_rtl:1.0 MEM_PORT_B DIN" *)
    input [MEM_WIDTH-1:0] din_b,
(* X_INTERFACE_INFO = "xilinx.com:interface:bram_rtl:1.0 MEM_PORT_B DOUT" *)
    output [MEM_WIDTH-1:0] dout_b
);

// Verilog-2001 wrapper. Vivado refuses a SystemVerilog file as the top of a BD
// module reference ("[filemgmt 56-195] ... not allowed as the top file in the
// reference"), but sub-modules may be SystemVerilog -- and the sparse backing
// store needs an associative array. So the BD sees this wrapper and the storage
// lives in sim_mem_core.sv.
sim_mem_core #(
    .MEM_WIDTH     (MEM_WIDTH),
    .MEM_DEPTH_LOG (MEM_DEPTH_LOG),
    .READ_LATENCY  (READ_LATENCY)
) u_core (
    .clk_a (clk_a), .rst_a (rst_a), .en_a (en_a), .addr_a (addr_a),
    .we_a  (we_a),  .din_a (din_a), .dout_a (dout_a),
    .clk_b (clk_b), .rst_b (rst_b), .en_b (en_b), .addr_b (addr_b),
    .we_b  (we_b),  .din_b (din_b), .dout_b (dout_b)
);

endmodule
