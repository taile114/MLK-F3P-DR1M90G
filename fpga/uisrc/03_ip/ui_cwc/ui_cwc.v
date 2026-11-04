/************************************************************\
**	Copyright (c) 2012-2026 Anlogic Inc.
**	All Right Reserved.
\************************************************************/
/************************************************************\
**	Gen time	:	Oct 16 2026 23:11:18
**	IpTool version	:	2026.08(208212 2026/08/14)
**	App version	:	Release_2026.2
**	Ip Version	:	1.0
\************************************************************/
module ui_cwc
(
  input   [15:0]                probe0,
  input   [0:0]                 probe1,
  input   [0:0]                 probe2,
  input   [15:0]                probe3,
  input   [15:0]                probe4,
  input   [15:0]                probe5,
  input   [0:0]                 probe6,
  input   [0:0]                 probe7,
  input   [7:0]                 probe8,
  input   [15:0]                probe9,
  input   [0:0]                 probe10,
  input   [0:0]                 probe11,
  input   [0:0]                 probe12,
  input   [2:0]                 probe13,
  input                         clk
);

  ui_cwc_ChipWatcher  ChipWatcher_Inst
  (
      .probe0(probe0),
      .probe1(probe1),
      .probe2(probe2),
      .probe3(probe3),
      .probe4(probe4),
      .probe5(probe5),
      .probe6(probe6),
      .probe7(probe7),
      .probe8(probe8),
      .probe9(probe9),
      .probe10(probe10),
      .probe11(probe11),
      .probe12(probe12),
      .probe13(probe13),
      .clk(clk)
  );
endmodule
