FROM python:3.11-slim
WORKDIR /app
COPY app.py .
RUN pip install flask paho-mqtt
CMD ["python", "app.py"]