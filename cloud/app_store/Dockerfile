FROM python:3.13-slim
WORKDIR /app
COPY cloud/app_store/requirements-lock.txt /tmp/requirements.txt
RUN pip install --no-cache-dir -r /tmp/requirements.txt && useradd -u 10001 -m store
COPY cloud /app/cloud
RUN mkdir -p /data && chown store:store /data
USER store
ENV STORE_DATABASE=/data/store.sqlite3
EXPOSE 8080
CMD ["python", "-m", "uvicorn", "cloud.app_store.app:app", "--host", "0.0.0.0", "--port", "8080", "--no-access-log"]
