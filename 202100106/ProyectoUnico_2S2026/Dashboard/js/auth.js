'use strict';

(() => {
	const formulario = document.getElementById('login-formulario');

	if (formulario) {
		const entradaUsuario = document.getElementById('login-usuario');
		const entradaPassword = document.getElementById('login-password');
		const mensajeError = document.getElementById('login-error');

		formulario.addEventListener('submit', async (evento) => {
			evento.preventDefault();
			if (mensajeError) {
				mensajeError.textContent = '';
			}

			const usuario = entradaUsuario ? entradaUsuario.value.trim() : '';
			const password = entradaPassword ? entradaPassword.value : '';

			try {
				const respuesta = await api.post('/api/login', { usuario, password });
				const datos = await respuesta.json().catch(() => ({}));

				if (respuesta.status === 200) {
					location.href = '/';
					return;
				}

				if (mensajeError) {
					mensajeError.textContent = datos.error || 'Credenciales inválidas';
				}
				if (entradaPassword) {
					entradaPassword.value = '';
				}
			} catch (error) {
				if (mensajeError) {
					mensajeError.textContent = 'Error de conexión con el daemon';
				}
				if (entradaPassword) {
					entradaPassword.value = '';
				}
			}
		});
	}
})();

async function iniciarSesionUI(datos) {
	if (!datos) {
		try {
			const respuesta = await api.sesion();
			if (respuesta.ok) {
				datos = await respuesta.json();
			}
		} catch (error) {
			return;
		}
	}
	if (!datos) {
		return;
	}

	const elementoUsuario = document.getElementById('sesion-usuario');
	const elementoRol = document.getElementById('sesion-rol');
	const botonLogout = document.getElementById('boton-logout');

	if (elementoUsuario) {
		elementoUsuario.textContent = datos.usuario || '—';
	}
	if (elementoRol) {
		elementoRol.textContent = datos.rol || '—';
	}

	if (botonLogout && !botonLogout.dataset.conectado) {
		botonLogout.dataset.conectado = 'true';
		botonLogout.addEventListener('click', async () => {
			try {
				await api.post('/api/logout');
			} finally {
				location.href = '/login.html';
			}
		});
	}

	if (datos.rol === 'common_user') {
		document.querySelectorAll('[data-rol="admin_user"]').forEach((elemento) => {
			elemento.style.display = 'none';
		});
	}
	if (typeof procesos !== 'undefined' && typeof procesos.renderizar === 'function') {
		procesos.renderizar();
	}
	if (typeof archivos !== 'undefined' && typeof archivos.renderizar === 'function') {
		archivos.renderizar();
	}
	if (datos.rol === 'admin_user') {
		if (typeof antivirus !== 'undefined') {
			if (typeof antivirus.cargarCuarentena === 'function') antivirus.cargarCuarentena();
			if (typeof antivirus.cargarBlacklist === 'function') antivirus.cargarBlacklist();
		}
		if (typeof logwatch !== 'undefined' && typeof logwatch.cargarLogwatch === 'function') {
			logwatch.cargarLogwatch();
		}
	}
}

window.auth = {
	esAdmin() {
		const el = document.getElementById('sesion-rol');
		return el ? el.textContent === 'admin_user' : false;
	}
};
