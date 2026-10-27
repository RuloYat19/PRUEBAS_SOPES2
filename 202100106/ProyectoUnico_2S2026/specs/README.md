# Specs — Hoja de ruta

Cada feature sigue el flujo **requirements → design → tasks → implementación → review**. Reglas del implementador en [`../AGENTS.md`](../AGENTS.md).

| # | Feature | Carpeta | Depende de | Estado |
|---|---|---|---|---|
| 00 | Fundación desde P4 | [`00-fundacion/`](00-fundacion/) | — | Aprobada |
| 01 | Autenticación y roles (PAM) | [`01-autenticacion/`](01-autenticacion/) | 00 | Aprobada |
| 02 | Motor de alertas y tiempo real | [`02-alertas/`](02-alertas/) | 00, 01 | Aprobada |
| 03 | Métricas del sistema | [`03-metricas/`](03-metricas/) | 01, 02 | Aprobada |
| 04 | Procesos (top 20, PID, kill) | [`04-procesos/`](04-procesos/) | 01, 02, 03 | Aprobada |
| 05 | Sensor eBPF e integridad de archivos | [`05-sensor-ebpf/`](05-sensor-ebpf/) | 00, 01, 02 | Aprobada |
| 06 | Antivirus y cuarentena | [`06-antivirus/`](06-antivirus/) | 02, 05 | Aprobada |
| 07 | Logwatch e IPC | [`07-logwatch/`](07-logwatch/) | 01, 02 | Aprobada |
| 08 | Documentación y entrega | [`08-entrega/`](08-entrega/) | todas | Aprobada |

Estados: `Pendiente` → `Spec listo` → `En implementación` → `En revisión` → `Aprobada`.
