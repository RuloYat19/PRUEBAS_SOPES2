'use strict';

(async () => {
	const $ = (id) => document.getElementById(id);

	function indicador(id, texto, clase) {
		const el = $(id);
		if (!el) return;
		el.className = 'indicador ' + clase;
		if (el.lastElementChild) {
			el.lastElementChild.textContent = texto;
		}
	}

	function conectarEventos() {
		const flujo = new EventSource('/api/eventos');

		flujo.onopen = () => {
			alertas.limpiar();
			alertas.cargarHistorial();
			metricas.cargarHistorial();
			procesos.cargar();
			archivos.cargar();
			antivirus.cargarAmenazas();
			if (window.auth && window.auth.esAdmin()) {
				antivirus.cargarCuarentena();
				antivirus.cargarBlacklist();
				if (typeof logwatch !== 'undefined' && typeof logwatch.cargarLogwatch === 'function') {
					logwatch.cargarLogwatch();
				}
			}
			api.get('/api/monitoreo').then((r) => r.json()).then((d) => {
				if (d && typeof d.metricas === 'boolean')
					metricas.aplicarEstado(d.metricas);
				if (d && typeof d.procesos === 'boolean')
					procesos.aplicarEstado(d.procesos);
				archivos.aplicarEstado(d);
			}).catch(() => {});
			indicador('ind-eventos', 'Alertas: en vivo (SSE)', 'activo');
			indicador('ind-metricas', 'Métricas: en vivo (SSE)', 'activo');
		};
		flujo.addEventListener('alerta', (m) => {
			const a = JSON.parse(m.data);
			alertas.agregar(a, true);
			antivirus.alAlerta(a);
		});
		flujo.addEventListener('metricas', (m) => metricas.agregar(JSON.parse(m.data)));
		flujo.addEventListener('procesos', (m) => procesos.aplicar(JSON.parse(m.data)));
		flujo.addEventListener('archivo', (m) => archivos.aplicarArchivo(JSON.parse(m.data)));
		flujo.addEventListener('archivos', (m) => archivos.aplicarLista(JSON.parse(m.data)));
		flujo.addEventListener('amenaza', (m) => antivirus.agregarAmenaza(JSON.parse(m.data)));
		flujo.addEventListener('monitoreo', (m) => {
			const d = JSON.parse(m.data);
			if (d && typeof d.metricas === 'boolean')
				metricas.aplicarEstado(d.metricas);
			if (d && typeof d.procesos === 'boolean')
				procesos.aplicarEstado(d.procesos);
			archivos.aplicarEstado(d);
		});
		flujo.onerror = () => {
			indicador('ind-eventos', 'Alertas: reconectando…', 'caido');
			indicador('ind-metricas', 'Métricas: reconectando…', 'caido');
			api.sesion();
		};
	}

	async function consultarPid(pid) {
		const resultado = $('pid-resultado');
		const error = (texto) => {
			const p = document.createElement('p');

			p.className = 'error';
			p.textContent = texto;
			resultado.replaceChildren(p);
		};

		$('pid-entrada').value = pid;
		try {
			const respuesta = await api.get(`/api/proceso?pid=${encodeURIComponent(pid)}`);
			const d = await respuesta.json();

			if (!respuesta.ok) {
				error(`PID ${pid}: ${d.error}`);
				return;
			}

			const numero = (n) => n.toLocaleString('es-GT');
			const dl = document.createElement('dl');
			const filas = [
				['Nombre', d.nombre],
				['PID', d.pid],
				['Tiempo de CPU', (d.runtime_ns / 1e6).toLocaleString('es-GT', { maximumFractionDigits: 2 }) + ' ms'],
				['Memoria virtual', numero(d.memory_kb) + ' KB']
			];
			for (const [termino, valor] of filas) {
				const dt = document.createElement('dt');
				const dd = document.createElement('dd');

				dt.textContent = termino;
				dd.textContent = valor;
				dl.append(dt, dd);
			}
			resultado.replaceChildren(dl);
		} catch (e) {
			error('No se pudo contactar al daemon.');
		}
	}

	window.consultarPid = consultarPid;

	$('pid-formulario').addEventListener('submit', (ev) => {
		ev.preventDefault();
		consultarPid($('pid-entrada').value);
	});

	try {
		const resp = await api.sesion();
		if (resp && resp.ok) {
			const datos = await resp.json();
			iniciarSesionUI(datos);
		}
	} catch (e) {
	}

	metricas.conectarInterruptor();
	procesos.conectarInterruptor();
	archivos.conectarControles();
	antivirus.conectarControles();
	if (typeof logwatch !== 'undefined' && typeof logwatch.conectarControles === 'function') {
		logwatch.conectarControles();
	}
	conectarEventos();
	setInterval(() => api.sesion(), 60000);
})();
