`timescale 1ns/1ps
module tb_alarm_sequencer;
    logic clk=0;
    always #5 clk=~clk;
    logic rst_n=0, clear_i=0, write_i=1;
    logic [5:0] state_i=0;
    wire alarm_o;
    wire [31:0] status_o;
    wire [5:0] state_o;
    integer checks=0;
    alarm_sequencer #(.CLK_FREQ_HZ(10)) dut(.*);
    task tick(input integer n);
        repeat(n) begin @(posedge clk); #1; end
    endtask
    task check(input bit condition, input string name);
        checks=checks+1;
        if (!condition) $fatal(1,"FAIL %s cycle=%t state=%h status=%h",name,$time,state_o,status_o);
    endtask
    task set_state(input logic [5:0] value);
        @(negedge clk); state_i=value; write_i=1; tick(1);
    endtask
    initial begin
        $dumpfile("alarm.vcd"); $dumpvars(0,tb_alarm_sequencer);
        tick(2); @(negedge clk); rst_n=1; tick(1);
        check(!alarm_o,"reset low");
        for (integer ch=0;ch<4;ch=ch+1) begin
            set_state(6'(1<<ch));
            for (integer n=0;n<10*(2*(ch+1)-1);n=n+1) begin
                check(alarm_o===((n/10)%2==0),"sensor one-second high/gap");
                tick(1);
            end
            check(!alarm_o && !status_o[1],"sensor exact pulse count");
            tick(20); check(!alarm_o,"unchanged state never retriggers");
            set_state(0); check(!alarm_o,"healthy low");
        end
        for (integer grade=1;grade<=3;grade=grade+1) begin
            set_state(6'(grade<<4));
            for(integer n=0;n<30*grade;n=n+1) begin
                check(alarm_o && status_o[2],"model exact long high"); tick(1);
            end
            check(!alarm_o && !status_o[1],"model exact duration");
            tick(30); check(!alarm_o,"model heartbeat no retrigger");
            set_state(0);
        end
        set_state(8); tick(3); set_state(6'h28);
        check(alarm_o && status_o[2],"model preempts sensor");
        tick(12); check(alarm_o,"model has no short-pulse gap");
        set_state(6'h38); tick(89); check(alarm_o,"escalation restarts full nine seconds");
        tick(1); check(!alarm_o,"escalation completes");
        tick(5); check(!alarm_o,"channels suppressed during model episode");
        set_state(8); tick(11); check(alarm_o && !status_o[2],"channels resume after model healthy");
        set_state(0); check(!alarm_o,"withdraw active sensor");
        tick(12); set_state(3);
        for (integer n=0;n<50;n=n+1) begin
            check(alarm_o===((n/10)%2==0),"queued groups retain full low gap"); tick(1);
        end
        check(!alarm_o,"queue exact pulse count");
        set_state(0); tick(12);
        set_state(15); tick(2); set_state(1); tick(80);
        check(!alarm_o,"withdraw pending sensors");
        set_state(0); set_state(6'h30); tick(2);
        @(negedge clk); write_i=0; tick(20);
        check(!alarm_o && status_o[3] && state_o==0,"lease expiry forces low");
        set_state(6'h30); check(alarm_o,"lease recovery rearms");
        @(negedge clk); clear_i=1; tick(1);
        check(!alarm_o && state_o==0,"soft reset forces low");
        $display("PASS alarm_sequencer checks=%0d",checks); $finish;
    end
    initial begin #100000; $fatal(1,"FAIL timeout"); end
endmodule
