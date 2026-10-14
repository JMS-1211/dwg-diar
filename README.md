# dwg-diar

돼굴돼굴(제주시청 업무 비서)의 **발화자 구분 실행 파일**을 만드는 저장소예요.

- `src/dwg-diar.cpp`: 녹음(16kHz · 한 채널 wav)을 읽어 '누가 언제 말했는지'를 구분해요. 인터넷을 쓰지 않고 PC 안에서만 동작해요.
- 바탕 기술: [sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx) (Apache-2.0)의 오프라인 발화자 구분 기능, onnxruntime (MIT)
- 빌드: `.github/workflows/build.yml`이 GitHub Actions 윈도우에서 sherpa-onnx 소스(태그 고정)를 받아 빌드하고, 한글 폴더에서 시험한 뒤 `build` 브랜치에 `dwg-diar.exe`와 SHA-256을 올려요.
- 모델: `.github/workflows/models.yml`이 sherpa-onnx 공식 배포처에서 받아 `models` 브랜치에 지문과 함께 올려요.
  - 구간 나누기: pyannote segmentation-3.0 (MIT)
  - 목소리 특징: 3D-Speaker CAM++ zh/en common advanced (Apache-2.0)

돼굴돼굴 본체 소스, 녹음 파일, 받아쓰기 결과는 이 저장소에 올리지 않아요.
