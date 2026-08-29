@echo off
REM Double-click to run in diagnostic mode: the bar renders as flat opaque
REM magenta instead of glass.
REM
REM Worth using because the real fill is 5% white, which is almost invisible on
REM its own - so a correct render and a silently broken one look much the same.
REM Magenta is unambiguous.
"%~dp0run.cmd" --diagnostic
