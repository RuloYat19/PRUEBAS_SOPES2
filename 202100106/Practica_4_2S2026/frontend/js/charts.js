'use strict';

const Graficos = (() => {
	const SVG = 'http://www.w3.org/2000/svg';
	const tooltip = document.getElementById('tooltip');

	function nodo(nombre, atributos, padre) {
		const n = document.createElementNS(SVG, nombre);
		for (const [clave, valor] of Object.entries(atributos || {}))
			n.setAttribute(clave, valor);
		if (padre)
			padre.appendChild(n);
		return n;
	}

	function mostrarTooltip(titulo, filas, x, y) {
		const cabecera = document.createElement('div');
		cabecera.textContent = titulo;
		tooltip.replaceChildren(cabecera);

		for (const fila of filas) {
			const div = document.createElement('div');
			const muestra = document.createElement('span');
			const etiqueta = document.createElement('span');
			const valor = document.createElement('b');

			div.className = 'fila';
			muestra.className = 'muestra';
			muestra.style.background = fila.color;
			etiqueta.textContent = fila.etiqueta;
			valor.textContent = fila.valor;
			div.append(muestra, etiqueta, valor);
			tooltip.appendChild(div);
		}

		tooltip.hidden = false;
		const ancho = tooltip.offsetWidth;
		const alto = tooltip.offsetHeight;
		tooltip.style.left = Math.min(x + 14, window.innerWidth - ancho - 8) + 'px';
		tooltip.style.top = Math.min(y + 14, window.innerHeight - alto - 8) + 'px';
	}

	function ocultarTooltip() {
		tooltip.hidden = true;
	}

	function leyenda(lista, elementos) {
		lista.replaceChildren(...elementos.map((e) => {
			const li = document.createElement('li');
			const muestra = document.createElement('span');
			const texto = document.createElement('span');

			muestra.className = 'muestra';
			muestra.style.background = e.color;
			texto.textContent = e.etiqueta;
			li.append(muestra, texto);
			if (e.valor !== undefined) {
				const valor = document.createElement('b');
				valor.textContent = e.valor;
				li.appendChild(valor);
			}
			if (e.detalle) {
				const detalle = document.createElement('small');
				detalle.textContent = e.detalle;
				li.appendChild(detalle);
			}
			return li;
		}));
	}

	function arco(cx, cy, exterior, interior, inicio, fin) {
		const p = (radio, angulo) =>
			(cx + radio * Math.cos(angulo)).toFixed(2) + ' ' +
			(cy + radio * Math.sin(angulo)).toFixed(2);
		const largo = fin - inicio > Math.PI ? 1 : 0;

		return `M ${p(exterior, inicio)} A ${exterior} ${exterior} 0 ${largo} 1 ${p(exterior, fin)} ` +
		       `L ${p(interior, fin)} A ${interior} ${interior} 0 ${largo} 0 ${p(interior, inicio)} Z`;
	}

	function dona(contenedor, segmentos, centro) {
		let st = contenedor._dona;

		if (!st) {
			const svg = nodo('svg', { viewBox: '0 0 200 200', role: 'img' }, contenedor);

			st = contenedor._dona = { svg, arcos: [], datos: [] };
			st.valor = nodo('text', { x: 100, y: 102, 'text-anchor': 'middle', class: 'centro-valor' }, svg);
			st.etiqueta = nodo('text', { x: 100, y: 120, 'text-anchor': 'middle', class: 'centro-etiqueta' }, svg);
		}

		const total = segmentos.reduce((suma, s) => suma + s.valor, 0) || 1;
		const separacion = 0.025;
		let angulo = -Math.PI / 2;

		st.datos = segmentos;
		segmentos.forEach((s, i) => {
			const barrido = (s.valor / total) * 2 * Math.PI;
			const inicio = angulo + separacion / 2;
			const fin = Math.min(angulo + barrido - separacion / 2, inicio + 2 * Math.PI - 0.001);

			if (!st.arcos[i]) {
				st.arcos[i] = nodo('path', {}, st.svg);
				st.arcos[i].addEventListener('mousemove', (ev) => {
					const d = st.datos[i];
					mostrarTooltip(d.etiqueta, [{ color: d.color, etiqueta: d.detalle, valor: d.texto }],
						       ev.clientX, ev.clientY);
				});
				st.arcos[i].addEventListener('mouseleave', ocultarTooltip);
			}
			st.arcos[i].style.fill = s.color;
			st.arcos[i].setAttribute('d', fin > inicio ? arco(100, 100, 96, 62, inicio, fin) : '');
			angulo += barrido;
		});

		st.valor.textContent = centro.valor;
		st.etiqueta.textContent = centro.etiqueta;
		st.svg.setAttribute('aria-label', segmentos.map((s) => `${s.etiqueta}: ${s.texto}`).join(', '));
	}

	function escalaY(maximo) {
		const bruto = (maximo > 0 ? maximo : 1) / 4;
		const potencia = Math.pow(10, Math.floor(Math.log10(bruto)));
		const paso = [1, 2, 2.5, 5, 10].map((m) => m * potencia).find((p) => p >= bruto);

		return { paso, tope: Math.ceil((maximo > 0 ? maximo : 1) / paso) * paso };
	}

	function hora(t) {
		return new Date(t).toLocaleTimeString('es-GT', { hour12: false });
	}

	function dibujarLineas(contenedor) {
		const st = contenedor._lineas;
		const ancho = contenedor.clientWidth;
		const alto = contenedor.clientHeight;
		const margen = { izq: 40, der: 58, sup: 10, inf: 22 };
		const tiempos = st.tiempos;

		contenedor.replaceChildren();
		if (!ancho || !alto || !tiempos.length)
			return;

		const svg = nodo('svg', { viewBox: `0 0 ${ancho} ${alto}`, role: 'img', 'aria-label': st.descripcion }, contenedor);
		const maximo = Math.max(...st.series.flatMap((s) => s.valores));
		const { paso, tope } = escalaY(maximo);
		const t0 = tiempos[0];
		const rango = tiempos[tiempos.length - 1] - t0 || 1;
		const x = (t) => margen.izq + ((t - t0) / rango) * (ancho - margen.izq - margen.der);
		const y = (v) => alto - margen.inf - (v / tope) * (alto - margen.sup - margen.inf);

		for (let v = 0; v <= tope + paso / 2; v += paso) {
			nodo('line', {
				x1: margen.izq, x2: ancho - margen.der, y1: y(v), y2: y(v),
				style: `stroke: var(${v === 0 ? '--eje' : '--rejilla'})`
			}, svg);
			nodo('text', { x: margen.izq - 8, y: y(v) + 4, 'text-anchor': 'end' }, svg)
				.textContent = String(+v.toFixed(2));
		}

		const marcas = tiempos.length > 1 ? [[0, 'start'], [tiempos.length - 1, 'end']] : [[0, 'start']];
		marcas.forEach(([i, ancla]) => {
			nodo('text', { x: x(tiempos[i]), y: alto - 4, 'text-anchor': ancla }, svg)
				.textContent = hora(tiempos[i]);
		});

		const extremos = st.series.map((s) => y(s.valores[s.valores.length - 1]));
		if (extremos.length === 2 && Math.abs(extremos[0] - extremos[1]) < 14) {
			const arriba = extremos[0] <= extremos[1] ? 0 : 1;
			const medio = (extremos[0] + extremos[1]) / 2;
			extremos[arriba] = medio - 7;
			extremos[1 - arriba] = medio + 7;
		}

		st.series.forEach((s, i) => {
			const puntos = s.valores.map((v, j) => `${x(tiempos[j]).toFixed(1)},${y(v).toFixed(1)}`);
			const ultimoX = x(tiempos[tiempos.length - 1]);

			nodo('polyline', {
				points: puntos.join(' '), fill: 'none', 'stroke-width': 2,
				'stroke-linejoin': 'round', 'stroke-linecap': 'round',
				style: `stroke: ${s.color}`
			}, svg);
			nodo('circle', {
				cx: ultimoX, cy: y(s.valores[s.valores.length - 1]), r: 4, 'stroke-width': 2,
				style: `fill: ${s.color}; stroke: var(--superficie)`
			}, svg);
			nodo('text', { x: ultimoX + 9, y: extremos[i] + 4, class: 'extremo' }, svg)
				.textContent = s.corta;
		});

		if (!st.cursor)
			return;

		const caja = contenedor.getBoundingClientRect();
		const objetivo = st.cursor.x - caja.left;
		let cercano = 0;
		tiempos.forEach((t, i) => {
			if (Math.abs(x(t) - objetivo) < Math.abs(x(tiempos[cercano]) - objetivo))
				cercano = i;
		});

		nodo('line', {
			x1: x(tiempos[cercano]), x2: x(tiempos[cercano]), y1: margen.sup, y2: alto - margen.inf,
			style: 'stroke: var(--tinta-3)', 'stroke-dasharray': '3 3'
		}, svg);
		st.series.forEach((s) => nodo('circle', {
			cx: x(tiempos[cercano]), cy: y(s.valores[cercano]), r: 4, 'stroke-width': 2,
			style: `fill: ${s.color}; stroke: var(--superficie)`
		}, svg));

		mostrarTooltip(hora(tiempos[cercano]), st.series.map((s) => ({
			color: s.color, etiqueta: s.etiqueta, valor: st.formato(s.valores[cercano])
		})), st.cursor.x, st.cursor.y);
	}

	function lineas(contenedor, tiempos, series, formato, descripcion) {
		let st = contenedor._lineas;

		if (!st) {
			st = contenedor._lineas = { cursor: null };
			contenedor.addEventListener('mousemove', (ev) => {
				st.cursor = { x: ev.clientX, y: ev.clientY };
				dibujarLineas(contenedor);
			});
			contenedor.addEventListener('mouseleave', () => {
				st.cursor = null;
				ocultarTooltip();
				dibujarLineas(contenedor);
			});
			window.addEventListener('resize', () => dibujarLineas(contenedor));
		}

		Object.assign(st, { tiempos, series, formato, descripcion });
		dibujarLineas(contenedor);
	}

	return { dona, lineas, leyenda };
})();
