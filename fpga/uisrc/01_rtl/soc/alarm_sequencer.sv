`default_nettype none
// Active-high J12.11/U7 alarm. Same clock as the AXI register bank.
// Repeated state writes renew the lease, never restart an unchanged episode.
module alarm_sequencer #(
    parameter integer CLK_FREQ_HZ = 100_000_000,
    parameter integer LEASE_SECONDS = 2
)(
    input  wire logic clk,
    input  wire logic rst_n,
    input  wire logic clear_i,
    input  wire logic write_i,
    input  wire logic [5:0] state_i, // [3:0] temp/current/vibration/speed, [5:4] grade
    output logic alarm_o,
    output logic [31:0] status_o,
    output logic [5:0] state_o
);
    localparam integer PHASE_W = $clog2(CLK_FREQ_HZ + 1);
    localparam integer LEASE_CYCLES = CLK_FREQ_HZ * LEASE_SECONDS;
    localparam integer LEASE_W = $clog2(LEASE_CYCLES + 1);
    typedef enum logic [5:0] {
        IDLE        = 6'b000001,
        SENSOR_HIGH = 6'b000010,
        SENSOR_GAP  = 6'b000100,
        MODEL_HIGH  = 6'b001000,
        MODEL_HOLD  = 6'b010000,
        COOLDOWN    = 6'b100000
    } phase_t;
    phase_t phase_state, next_phase_state;
    logic [PHASE_W-1:0] phase, next_phase;
    logic [LEASE_W-1:0] lease, next_lease;
    logic [3:0] seen, pending, active, next_seen, next_pending, next_active;
    logic [1:0] model_seen, next_model_seen;
    logic [3:0] seconds_left, next_seconds_left;
    logic [2:0] pulses_left, next_pulses_left;
    logic expired, next_expired;
    logic [5:0] desired, next_state;
    logic [3:0] eligible, selected;
    logic [2:0] selected_count;
    logic phase_done;

    always_comb begin
        desired = write_i ? state_i : state_o;
        eligible = (pending | (desired[3:0] & ~seen)) & desired[3:0];
        selected = 4'b0000;
        selected_count = 3'd0;
        if (eligible[0]) begin selected = 4'b0001; selected_count = 3'd1; end
        else if (eligible[1]) begin selected = 4'b0010; selected_count = 3'd2; end
        else if (eligible[2]) begin selected = 4'b0100; selected_count = 3'd3; end
        else if (eligible[3]) begin selected = 4'b1000; selected_count = 3'd4; end
        phase_done = phase == PHASE_W'(CLK_FREQ_HZ - 1);
    end

    always_comb begin
        next_phase_state = phase_state;
        next_phase = phase;
        next_lease = write_i ? LEASE_W'(LEASE_CYCLES - 1) : lease - 1'b1;
        next_state = desired;
        next_seen = desired[3:0];
        next_pending = eligible;
        next_active = active;
        next_model_seen = desired[5:4];
        next_seconds_left = seconds_left;
        next_pulses_left = pulses_left;
        next_expired = write_i ? 1'b0 : expired;
        if (!write_i && lease == 0) begin
            next_phase_state = IDLE; next_phase = '0; next_lease = '0; next_state = '0;
            next_seen = '0; next_pending = '0; next_active = '0; next_model_seen = '0;
            next_seconds_left = '0; next_pulses_left = '0; next_expired = 1'b1;
        end else if (desired[5:4] != 0) begin
            // A continuous model episode suppresses every channel, even after
            // its long pulse completed. Only entry/escalation starts a pulse.
            next_seen = '0; next_pending = '0; next_active = '0;
            if (model_seen == 0 || desired[5:4] > model_seen) begin
                next_phase_state = MODEL_HIGH; next_phase = '0;
                next_seconds_left = {2'b00, desired[5:4]} * 4'd3;
                next_pulses_left = '0;
            end else if (phase_state == MODEL_HIGH) begin
                if (phase_done) begin
                    next_phase = '0;
                    if (seconds_left == 1) begin
                        next_phase_state = MODEL_HOLD; next_seconds_left = '0;
                    end else next_seconds_left = seconds_left - 1'b1;
                end else next_phase = phase + 1'b1;
            end else next_phase_state = MODEL_HOLD;
        end else begin
            case (phase_state)
                IDLE: begin
                    next_phase = '0;
                    if (selected != 0) begin
                        next_phase_state = SENSOR_HIGH;
                        next_active = selected; next_pending = eligible & ~selected;
                        next_pulses_left = selected_count;
                    end
                end
                MODEL_HIGH, MODEL_HOLD: begin
                    next_phase_state = COOLDOWN; next_phase = '0;
                end
                SENSOR_HIGH, SENSOR_GAP: begin
                    if ((active & desired[3:0]) == 0) begin
                        next_phase_state = COOLDOWN; next_phase = '0; next_active = '0;
                    end else if (phase_done) begin
                        next_phase = '0;
                        if (phase_state == SENSOR_GAP) next_phase_state = SENSOR_HIGH;
                        else if (pulses_left == 1) begin
                            next_phase_state = COOLDOWN; next_active = '0;
                        end else begin
                            next_phase_state = SENSOR_GAP; next_pulses_left = pulses_left - 1'b1;
                        end
                    end else next_phase = phase + 1'b1;
                end
                COOLDOWN: begin
                    // A full low second separates channel groups and model
                    // recovery; otherwise adjacent highs merge audibly.
                    if (phase_done) begin
                        next_phase = '0;
                        if (selected != 0) begin
                            next_phase_state = SENSOR_HIGH;
                            next_active = selected; next_pending = eligible & ~selected;
                            next_pulses_left = selected_count;
                        end else next_phase_state = IDLE;
                    end else next_phase = phase + 1'b1;
                end
                default: begin
                    next_phase_state = IDLE; next_phase = '0; next_active = '0;
                end
            endcase
        end
    end

    always_ff @(posedge clk) begin
        if (!rst_n || clear_i) begin
            phase_state <= IDLE; phase <= '0; lease <= '0; state_o <= '0;
            seen <= '0; pending <= '0; active <= '0; model_seen <= '0;
            seconds_left <= '0; pulses_left <= '0; expired <= 1'b0; alarm_o <= 1'b0;
        end else begin
            phase_state <= next_phase_state; phase <= next_phase; lease <= next_lease;
            state_o <= next_state; seen <= next_seen; pending <= next_pending;
            active <= next_active; model_seen <= next_model_seen;
            seconds_left <= next_seconds_left; pulses_left <= next_pulses_left; expired <= next_expired;
            alarm_o <= next_phase_state == SENSOR_HIGH || next_phase_state == MODEL_HIGH;
        end
    end

    always_comb begin
        status_o = {16'd0, active, pending, 4'd0, expired,
            (phase_state == MODEL_HIGH || phase_state == MODEL_HOLD),
            (phase_state == SENSOR_HIGH || phase_state == SENSOR_GAP || phase_state == MODEL_HIGH), alarm_o};
    end
endmodule
`default_nettype wire
