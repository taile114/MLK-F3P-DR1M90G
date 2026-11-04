`timescale 1ns/1ps
module tb_npu_hp1_write_mux;
    logic  clk;
    logic  rst_n;
    logic [1:0][5:0]     s_awid;
    logic [1:0][32:0]    s_awaddr;
    logic [1:0][3:0]     s_awlen;
    logic [1:0][1:0]     s_awsize;
    logic [1:0][1:0]     s_awburst;
    logic [1:0]          s_awlock;
    logic [1:0][3:0]     s_awcache;
    logic [1:0][2:0]     s_awprot;
    logic [1:0]          s_awvalid;
    logic [1:0]          s_awready;
    logic [1:0][63:0]    s_wdata;
    logic [1:0][7:0]     s_wstrb;
    logic [1:0]          s_wlast;
    logic [1:0]          s_wvalid;
    logic [1:0]          s_wready;
    logic [1:0][5:0]     s_bid;
    logic [1:0][1:0]     s_bresp;
    logic [1:0]          s_bvalid;
    logic [1:0]          s_bready;
    logic [5:0]          m_awid;
    logic [32:0]         m_awaddr;
    logic [3:0]          m_awlen;
    logic [1:0]          m_awsize;
    logic [1:0]          m_awburst;
    logic  m_awlock;
    logic [3:0]          m_awcache;
    logic [2:0]          m_awprot;
    logic  m_awvalid;
    logic  m_awready;
    logic [63:0]         m_wdata;
    logic [7:0]          m_wstrb;
    logic  m_wlast;
    logic  m_wvalid;
    logic  m_wready;
    logic [5:0]          m_bid;
    logic [1:0]          m_bresp;
    logic  m_bvalid;
    logic  m_bready;

    npu_hp1_write_mux dut (.*);
    initial clk = 0;
    always #5 clk = ~clk;
    int cycle = 0;
    int completed = 0;
    int beats_left = 0;
    int beat_index = 0;
    int active_source = 0;
    int active_tx = 0;
    int response_delay = 0;
    logic response_pending = 0;
    logic [5:0] saved_id;
    logic [1:0] saved_resp;
    logic aw_stalled = 0;
    logic w_stalled = 0;
    logic b_stalled = 0;
    logic [55:0] held_aw;
    logic [73:0] held_w;
    logic [7:0] held_b;

    function automatic logic [63:0] payload(input int p, tx, beat);
        return 64'h1234000000000000 | (64'(p) << 16) | (64'(tx) << 8) | 64'(beat);
    endfunction

    // Both sources keep AW and W independent and hold them through stalls.
    // Sixteen-beat writes, six-bit IDs and SLVERR responses exercise the HP port.
    task automatic send_writes(input int p);
        int count;
        for (int tx = 0; tx < 8; tx++) begin
            count = tx == 7 ? 16 : 1 + p + tx % 5;
            @(negedge clk);
            s_awid[p] = p == 0 ? 6'd0 : 6'd63;
            s_awaddr[p] = 33'h1000 + (33'(p) << 16) + (33'(tx) << 8);
            s_awlen[p] = 4'(count - 1);
            s_awsize[p] = 2'b11;
            s_awburst[p] = 2'b01;
            s_awlock[p] = 0;
            s_awcache[p] = 4'(p);
            s_awprot[p] = 3'(tx);
            s_awvalid[p] = 1;
            s_wvalid[p] = 1;
            s_wdata[p] = payload(p, tx, 0);
            s_wstrb[p] = 8'hff;
            s_wlast[p] = count == 1;
            do @(posedge clk); while (!s_awready[p]);
            @(negedge clk);
            s_awvalid[p] = 0;
            for (int beat = 0; beat < count; beat++) begin
                s_wdata[p] = payload(p, tx, beat);
                s_wlast[p] = beat == count - 1;
                do @(posedge clk); while (!s_wready[p]);
                @(negedge clk);
            end
            s_wvalid[p] = 0;
            // Hold BREADY low even after the target has asserted BVALID.
            repeat (10 + p) @(negedge clk);
            s_bready[p] = 1;
            do @(posedge clk); while (!s_bvalid[p]);
            if (s_bid[p] !== (p == 0 ? 6'd0 : 6'd63) ||
                s_bresp[p] !== (tx % 2 ? 2'b10 : 2'b00))
                $fatal(1, "wrong response source=%0d tx=%0d", p, tx);
            @(negedge clk);
            s_bready[p] = 0;
        end
    endtask

    // DDR model with AW/W/B backpressure and a scoreboard independent of the
    // mux implementation. Each accepted burst must remain with its AW owner.
    always @(negedge clk) begin
        m_awready = rst_n && cycle % 4 == 1 && beats_left == 0 && !response_pending;
        m_wready = rst_n && cycle % 3 != 1 && beats_left != 0;
    end
    always @(posedge clk) begin
        cycle = cycle + 1;
        if (rst_n) begin
            if (aw_stalled && {m_awid,m_awaddr,m_awlen,m_awsize,m_awburst,
                              m_awlock,m_awcache,m_awprot,m_awvalid} !== held_aw)
                $fatal(1, "AW changed under backpressure");
            if (w_stalled && {m_wdata,m_wstrb,m_wlast,m_wvalid} !== held_w)
                $fatal(1, "W changed under backpressure");
            if (b_stalled && {m_bid,m_bresp} !== held_b)
                $fatal(1, "B changed under backpressure");
            aw_stalled = m_awvalid && !m_awready;
            w_stalled = m_wvalid && !m_wready;
            b_stalled = m_bvalid && !m_bready;
            held_aw = {m_awid,m_awaddr,m_awlen,m_awsize,m_awburst,
                       m_awlock,m_awcache,m_awprot,m_awvalid};
            held_w = {m_wdata,m_wstrb,m_wlast,m_wvalid};
            held_b = {m_bid,m_bresp};
            if (s_awready == 2'b11 || s_wready == 2'b11 || s_bvalid == 2'b11)
                $fatal(1, "two writers selected");
            if (m_awvalid && m_awready) begin
                if (beats_left || response_pending) $fatal(1, "overlapping burst");
                active_source = int'(m_awaddr[16]);
                active_tx = int'(m_awaddr[11:8]);
                if (m_awid !== (active_source == 0 ? 6'd0 : 6'd63) ||
                    m_awsize !== 2'b11 || m_awburst !== 2'b01 ||
                    m_awcache !== 4'(active_source) || m_awprot !== 3'(active_tx))
                    $fatal(1, "AW metadata lost");
                if (completed == 0 && active_source != 0)
                    $fatal(1, "initial arbitration priority lost");
                if (active_source != completed % 2)
                    $fatal(1, "round-robin fairness lost");
                beats_left = int'(m_awlen) + 1;
                beat_index = 0;
                saved_id = m_awid;
                saved_resp = active_tx % 2 ? 2'b10 : 2'b00;
            end
            if (m_wvalid && m_wready) begin
                if (!beats_left || m_wdata !== payload(active_source,active_tx,beat_index) ||
                    m_wstrb !== 8'hff || m_wlast !== (beats_left == 1))
                    $fatal(1, "burst data/last interleaved: source=%0d tx=%0d beat=%0d",active_source,active_tx,beat_index);
                beats_left = beats_left - 1;
                beat_index = beat_index + 1;
                if (!beats_left) begin
                    response_pending = 1;
                    response_delay = 3;
                end
            end
            if (m_bvalid && m_bready) begin
                completed = completed + 1;
                response_pending = 0;
                m_bvalid <= 0;
            end else if (response_pending && !m_bvalid) begin
                if (response_delay) response_delay = response_delay - 1;
                else begin
                    m_bid <= saved_id;
                    m_bresp <= saved_resp;
                    m_bvalid <= 1;
                end
            end
        end
    end

    initial begin
        rst_n=0;
        s_awid='0; s_awaddr='0; s_awlen='0; s_awsize='0; s_awburst='0;
        s_awlock='0; s_awcache='0; s_awprot='0; s_awvalid='0;
        s_wdata='0; s_wstrb='0; s_wlast='0; s_wvalid='0; s_bready='0;
        m_awready=0; m_wready=0; m_bid=0; m_bresp=0; m_bvalid=0;
        repeat (4) @(negedge clk);
        if (m_awvalid || m_wvalid || m_bready || (|s_awready) || (|s_wready) || (|s_bvalid))
            $fatal(1, "bus active in reset");
        rst_n=1;
        fork
            send_writes(0);
            send_writes(1);
        join
        if (completed != 16) $fatal(1, "lost completion: %0d",completed);
        @(negedge clk);
        rst_n=0;
        repeat(2) @(negedge clk);
        if (m_awvalid || m_wvalid || m_bready) $fatal(1, "reset did not idle mux");
        $display("PASS: HP1 writes, IDs, bursts, arbitration, stalls, SLVERR, reset (%0d bursts)",completed);
        $finish;
    end
    initial begin
        #100000;
        $fatal(1, "HP1 write mux timeout");
    end
endmodule
