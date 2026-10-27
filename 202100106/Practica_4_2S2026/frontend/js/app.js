'use strict';

(() => {
	const API = (new URLSearchParams(location.search).get('api') || '').replace(/\/$/, '');
	const INTERVALO_MS = 2000;
	const MAX_PUNTOS = 90;
	const MAX_EVENTOS = 100;
	const KB_POR_GB = 1048576;

	const COLOR = {
		usada: 'var(--serie-usada)',
		cache: 'var(--serie-cache)',
		libre: 'var(--serie-libre)',
		swap: 'var(--serie-swap)'
	};

	const ESTADOS = {
		R: ['En ejecución', 'bueno'],
		S: ['Dormido', 'neutro'],
		I: ['Inactivo (kernel)', 'neutro'],
		D: ['Espera de E/S', 'neutro'],
		T: ['Detenido', 'neutro'],
		t: ['En traza', 'neutro'],
		Z: ['Zombie', 'critico'],
		X: ['Muerto', 'critico']
	};

	const $ = (id) => document.getElementById(id);
	const numero = (n) => n.toLocaleString('es-GT');
	const gb = (kb) => (kb / KB_POR_GB).toFixed(2) + ' GB';

	const historial = { tiempos: [], usada: [], swap: [] };
	const idsEventos = new Set();
	let procesos = [];
	let orden = { columna: 'pid', sentido: 'asc' };
	let historialPendiente = true;

	function indicador(id, texto, clase) {
		const el = $(id);

		el.className = 'indicador ' + clase;
		el.lastElementChild.textContent = texto;
	}

	function pintarMemoria(d) {
		const total = d.memoria_usada + d.memoria_libre;
		const cache = Math.min(d.memoria_cache, d.memoria_libre);
		const libre = d.memoria_libre - cache;
		const pct = (v) => (total ? Math.round((v / total) * 100) : 0) + ' %';
		const segmentos = [
			{ etiqueta: 'Usada', detalle: 'MemTotal − MemAvailable', valor: d.memoria_usada, color: COLOR.usada },
			{ etiqueta: 'Caché', detalle: 'Cached + Buffers', valor: cache, color: COLOR.cache },
			{ etiqueta: 'Libre', detalle: 'Disponible sin contar caché', valor: libre, color: COLOR.libre }
		];

		segmentos.forEach((s) => { s.texto = `${gb(s.valor)} (${pct(s.valor)})`; });
		$('mem-total').textContent = gb(total);
		Graficos.dona($('grafico-dona'), segmentos, { valor: pct(d.memoria_usada), etiqueta: 'en uso' });
		Graficos.leyenda($('leyenda-dona'), segmentos.map((s) => ({
			color: s.color, etiqueta: s.etiqueta, valor: gb(s.valor), detalle: pct(s.valor)
		})));

		historial.tiempos.push(Date.now());
		historial.usada.push(d.memoria_usada / KB_POR_GB);
		historial.swap.push(d.swap_usada / KB_POR_GB);
		for (const serie of Object.values(historial))
			if (serie.length > MAX_PUNTOS)
				serie.shift();

		Graficos.lineas($('grafico-lineas'), historial.tiempos, [
			{ etiqueta: 'Memoria física usada', corta: 'Física', color: COLOR.usada, valores: historial.usada },
			{ etiqueta: 'Swap usada', corta: 'Swap', color: COLOR.swap, valores: historial.swap }
		], (v) => v.toFixed(2) + ' GB', 'Memoria física usada y swap usada en el tiempo');
		Graficos.leyenda($('leyenda-lineas'), [
			{ color: COLOR.usada, etiqueta: 'Memoria física usada', valor: gb(d.memoria_usada) },
			{ color: COLOR.swap, etiqueta: 'Swap usada', valor: gb(d.swap_usada) }
		]);
	}

	function pintarResumen(r) {
		$('res-total').textContent = numero(r.total);
		$('res-running').textContent = numero(r.running);
		$('res-sleeping').textContent = numero(r.sleeping);
		$('res-zombie').textContent = numero(r.zombie);
	}

	function insignia(estado) {
		const [texto, clase] = ESTADOS[estado] || ['Estado ' + estado, 'neutro'];
		const span = document.createElement('span');
		const punto = document.createElement('i');

		span.className = 'insignia';
		punto.className = 'punto ' + clase;
		span.append(punto, `${texto} (${estado})`);
		return span;
	}

	function pintarProcesos() {
		const texto = $('filtro-texto').value.trim().toLowerCase();
		const estado = $('filtro-estado').value;
		const signo = orden.sentido === 'asc' ? 1 : -1;
		const visibles = procesos.filter((p) =>
			(!estado || p.estado === estado) &&
			(!texto || p.nombre.toLowerCase().includes(texto) || String(p.pid).includes(texto)));

		visibles.sort((a, b) => {
			const va = a[orden.columna];
			const vb = b[orden.columna];
			const comparacion = typeof va === 'number' ? va - vb : String(va).localeCompare(String(vb));
			return signo * (comparacion || a.pid - b.pid);
		});

		$('procesos-cuerpo').replaceChildren(...visibles.map((p) => {
			const tr = document.createElement('tr');

			tr.dataset.pid = p.pid;
			tr.insertCell().textContent = p.pid;
			tr.insertCell().textContent = p.nombre;
			tr.insertCell().appendChild(insignia(p.estado));
			return tr;
		}));
		$('procesos-contador').textContent = `${numero(visibles.length)} de ${numero(procesos.length)}`;
	}

	function sinMarcaDeTiempo(linea) {
		const prefijo = linea.match(/^\s*\[[^\]]*\d{1,2}:\d{2}[^\]]*\]\s*/);

		return prefijo && prefijo[0].length < linea.length ? linea.slice(prefijo[0].length) : linea;
	}

	function resaltar(texto, palabra) {
		const partes = palabra ? texto.split(palabra) : [texto];

		return partes.flatMap((parte, i) => {
			if (i === 0)
				return [parte];
			const marca = document.createElement('mark');
			marca.textContent = palabra;
			return [marca, parte];
		});
	}

	function agregarEvento(e, nuevo) {
		const cuerpo = $('eventos-cuerpo');
		const tr = document.createElement('tr');
		const palabra = document.createElement('span');
		const archivo = e.archivo_origen.split('/').pop();
		let celda;

		if (idsEventos.has(e.id))
			return;
		idsEventos.add(e.id);

		tr.dataset.id = e.id;
		if (nuevo)
			tr.className = 'nuevo';
		tr.insertCell().textContent = e.timestamp;
		palabra.className = 'insignia alerta';
		palabra.textContent = e.palabra_detectada;
		tr.insertCell().appendChild(palabra);
		celda = tr.insertCell();
		celda.textContent = archivo;
		celda.title = e.archivo_origen;
		celda = tr.insertCell();
		celda.className = 'contenido';
		celda.title = e.contenido;
		celda.append(...resaltar(sinMarcaDeTiempo(e.contenido), e.palabra_detectada));

		const siguiente = [...cuerpo.rows].find((fila) => Number(fila.dataset.id) < e.id);
		cuerpo.insertBefore(tr, siguiente || null);
		while (cuerpo.rows.length > MAX_EVENTOS) {
			idsEventos.delete(Number(cuerpo.lastElementChild.dataset.id));
			cuerpo.lastElementChild.remove();
		}
		contarEventos();
	}

	function contarEventos() {
		const cantidad = $('eventos-cuerpo').rows.length;

		$('eventos-contador').textContent = cantidad === 1 ? '1 evento' : `${cantidad} eventos`;
		$('eventos-vacio').classList.toggle('visible', cantidad === 0);
	}

	async function actualizar() {
		try {
			const respuesta = await fetch(API + '/api/estado', { cache: 'no-store' });
			if (!respuesta.ok)
				throw new Error(respuesta.status);
			const d = await respuesta.json();

			pintarResumen(d.resumen_procesos);
			pintarMemoria(d);
			procesos = d.procesos;
			pintarProcesos();
			if (historialPendiente) {
				historialPendiente = false;
				d.eventos_seguridad.forEach((e) => agregarEvento(e, false));
			}
			indicador('ind-metricas',
				  'Métricas: ' + new Date().toLocaleTimeString('es-GT', { hour12: false }), 'activo');
		} catch (error) {
			indicador('ind-metricas', 'Métricas: sin conexión con el daemon', 'caido');
		}
		setTimeout(actualizar, INTERVALO_MS);
	}

	function conectarEventos() {
		const flujo = new EventSource(API + '/api/eventos');

		flujo.onopen = () => {
			$('eventos-cuerpo').replaceChildren();
			idsEventos.clear();
			contarEventos();
			historialPendiente = true;
			indicador('ind-eventos', 'Eventos: en vivo (SSE)', 'activo');
		};
		flujo.onmessage = (mensaje) => agregarEvento(JSON.parse(mensaje.data), true);
		flujo.onerror = () => indicador('ind-eventos', 'Eventos: reconectando…', 'caido');
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
			const respuesta = await fetch(`${API}/api/proceso?pid=${encodeURIComponent(pid)}`, { cache: 'no-store' });
			const d = await respuesta.json();

			if (!respuesta.ok) {
				error(`PID ${pid}: ${d.error}`);
				return;
			}

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

	for (const [letra, [texto]] of Object.entries(ESTADOS))
		$('filtro-estado').add(new Option(`${texto} (${letra})`, letra));

	$('filtro-texto').addEventListener('input', pintarProcesos);
	$('filtro-estado').addEventListener('change', pintarProcesos);

	document.querySelectorAll('th button[data-columna]').forEach((boton) => {
		boton.addEventListener('click', () => {
			const columna = boton.dataset.columna;

			orden = {
				columna,
				sentido: orden.columna === columna && orden.sentido === 'asc' ? 'desc' : 'asc'
			};
			document.querySelectorAll('th button[data-orden]').forEach((b) => b.removeAttribute('data-orden'));
			boton.dataset.orden = orden.sentido;
			pintarProcesos();
		});
	});
	document.querySelector('th button[data-columna="pid"]').dataset.orden = 'asc';

	$('procesos-cuerpo').addEventListener('click', (ev) => {
		const fila = ev.target.closest('tr');

		if (fila) {
			consultarPid(fila.dataset.pid);
			$('panel-pid').scrollIntoView({ behavior: 'smooth', block: 'nearest' });
		}
	});

	$('pid-formulario').addEventListener('submit', (ev) => {
		ev.preventDefault();
		consultarPid($('pid-entrada').value);
	});

	contarEventos();
	conectarEventos();
	actualizar();
})();
