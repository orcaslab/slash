`timescale 1ns / 1ps

// Sparse memory model for the simulation platform.
//
// A dense array cannot span a real platform aperture: the V80 maps 32 GB, and a
// 64-bit Vortex addresses 8 GB of it, which as 64-bit words is 8 GB of
// simulator memory. Sizing the array down instead -- the original
// 2**MEM_DEPTH_LOG with MEM_DEPTH_LOG=26 -- is what capped the mapped window at
// 128 MB. That is fine for an HLS kernel, which only ever dereferences pointers
// the runtime handed it, and wrong for a processor: Vortex synthesises its own
// addresses and places the stack at the top of its memory map, so it touches
// the far end of the aperture from its first instruction. Everything below the
// window simply never gets an AXI response and the master stalls forever.
//
// An associative array allocates on write and costs nothing for untouched
// addresses -- the same address-agnostic behaviour XRT's hw_emu memory model
// has, which is why hw_emu can back a whole multi-GB bank.
module sim_mem_core #(
    parameter MEM_WIDTH     = 64,
    parameter MEM_DEPTH_LOG = 34,
    parameter READ_LATENCY  = 50
)(
    input                      clk_a, rst_a, en_a,
    input  [MEM_DEPTH_LOG-1:0] addr_a,
    input  [MEM_WIDTH/8-1:0]   we_a,
    input  [MEM_WIDTH-1:0]     din_a,
    output [MEM_WIDTH-1:0]     dout_a,
    input                      clk_b, rst_b, en_b,
    input  [MEM_DEPTH_LOG-1:0] addr_b,
    input  [MEM_WIDTH/8-1:0]   we_b,
    input  [MEM_WIDTH-1:0]     din_b,
    output [MEM_WIDTH-1:0]     dout_b
);

logic [MEM_WIDTH-1:0] mem [longint unsigned];

// Never-written addresses read as zero rather than X, so an uninitialised
// fetch is diagnosable instead of poisoning the whole design.
function automatic logic [MEM_WIDTH-1:0] mem_peek(input longint unsigned a);
    return mem.exists(a) ? mem[a] : '0;
endfunction

logic [MEM_WIDTH-1:0] delayline_a [READ_LATENCY-1:0];
logic [MEM_WIDTH-1:0] delayline_b [READ_LATENCY-1:0];

// Read before write within each port: an associative array needs
// read-modify-write for byte enables, and capturing the read first preserves
// the original nonblocking semantics, where a read concurrent with a write to
// the same address returns the old contents.
always @(posedge clk_a) begin : port_a
    logic [MEM_WIDTH-1:0] rd_a;
    rd_a = mem_peek(addr_a);
    if (rst_a)     delayline_a[0] <= '0;
    else if (en_a) delayline_a[0] <= rd_a;
    if (en_a && (|we_a)) begin
        if (!mem.exists(addr_a)) mem[addr_a] = '0;
        for (int bi = 0; bi < MEM_WIDTH/8; bi++)
            if (we_a[bi]) mem[addr_a][8*bi +: 8] = din_a[8*bi +: 8];
    end
end

always @(posedge clk_b) begin : port_b
    logic [MEM_WIDTH-1:0] rd_b;
    rd_b = mem_peek(addr_b);
    if (rst_b)     delayline_b[0] <= '0;
    else if (en_b) delayline_b[0] <= rd_b;
    if (en_b && (|we_b)) begin
        if (!mem.exists(addr_b)) mem[addr_b] = '0;
        for (int bi = 0; bi < MEM_WIDTH/8; bi++)
            if (we_b[bi]) mem[addr_b][8*bi +: 8] = din_b[8*bi +: 8];
    end
end

genvar i;
generate for (i = 1; i < READ_LATENCY; i = i + 1) begin : read_delay
    always @(posedge clk_a) if (rst_a) delayline_a[i] <= '0; else delayline_a[i] <= delayline_a[i-1];
    always @(posedge clk_b) if (rst_b) delayline_b[i] <= '0; else delayline_b[i] <= delayline_b[i-1];
end endgenerate

assign dout_a = delayline_a[READ_LATENCY-1];
assign dout_b = delayline_b[READ_LATENCY-1];

endmodule
