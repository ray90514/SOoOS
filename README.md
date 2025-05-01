# SOoOS
Simple Out of Order CPU Simulator

## Build
```
make
```

### Configuration

Some configurations are defined as macros.

```
2-wide superscalar for rename/dispatch and commit, RAT, icahe size 32
```

## Instructions

### Format

```
opcode $dst, $src1, $src2
```

```
opcode $op1, offset($op2)
```

### Supported Opcodes

```
add, sub, and, or, load(lb), store(sb)
```

## Validation

After simulation, it wil run a validation that compares reg and memory values from in-order execution.

## Screenshots

<img width="628" alt="Screenshot 2025-05-01 at 9 50 46 AM" src="https://github.com/user-attachments/assets/7a8ce2a2-59c5-459f-92a9-251dcfc6cf1f" />

<img width="628" alt="Screenshot 2025-05-01 at 9 39 45 AM" src="https://github.com/user-attachments/assets/dfaded33-bb23-414d-93ee-85a3708c08fe" />
