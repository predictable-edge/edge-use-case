#!/bin/bash

INTERRUPTS=(
  132 133 134 135 136 137 139 140 141 142 143 144 145 146 147 148 149 150 151 152 153 154 155 156
  163 164 165 166 167 168 169 170 171 172 173 174 175 176 177 178 179 180 181 182
)

AFFINITY_MASK="00C00C00"

echo "Stopping irqbalance..."
sudo systemctl stop irqbalance 2>/dev/null || echo "irqbalance service not found or already stopped."

echo "Binding interrupts to CPU mask: $AFFINITY_MASK"
for IRQ in "${INTERRUPTS[@]}"; do
  if [ -f /proc/irq/$IRQ/smp_affinity ]; then
    echo "Setting IRQ $IRQ to affinity mask $AFFINITY_MASK"
    echo $AFFINITY_MASK | sudo tee /proc/irq/$IRQ/smp_affinity > /dev/null
  else
    echo "IRQ $IRQ does not exist, skipping."
  fi
done

echo "Done. Verify bindings with: cat /proc/interrupts"