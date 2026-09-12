# Plan: Three node cycle

## Epic: cycle-epic -- An epic
- labels: priority:P2, epic:cycle-epic
- priority: P2
- track: Codebase
- status: Ready

### Issue: cyc-a -- First node
- labels: priority:P2, epic:cycle-epic
- priority: P2
- track: Codebase
- status: Ready
- depends-on: cyc-c

### Issue: cyc-b -- Second node
- labels: priority:P2, epic:cycle-epic
- priority: P2
- track: Codebase
- status: Ready
- depends-on: cyc-a

### Issue: cyc-c -- Third node
- labels: priority:P2, epic:cycle-epic
- priority: P2
- track: Codebase
- status: Ready
- depends-on: cyc-b
